// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "btree/backfill.hpp"

#include "btree/depth_first_traversal.hpp"
#include "btree/leaf_node.hpp"
#include "containers/archive/boost_types.hpp"
#include "containers/archive/stl_types.hpp"

RDB_IMPL_SERIALIZABLE_1_FOR_CLUSTER(backfill_pre_atom_t, range);
RDB_IMPL_SERIALIZABLE_3_FOR_CLUSTER(backfill_atom_t::pair_t, key, recency, value);
RDB_IMPL_SERIALIZABLE_3_FOR_CLUSTER(backfill_atom_t,
    range, pairs, min_deletion_timestamp);

void backfill_atom_t::mask_in_place(const key_range_t &m) {
    range = range.intersection(m);
    std::vector<pair_t> new_pairs;
    for (auto &&pair : pairs) {
        if (m.contains_key(pair.key)) {
            new_pairs.push_back(std::move(pair));
        }
    }
    pairs = std::move(new_pairs);
}

key_range_t::right_bound_t convert_right_bound(const btree_key_t *right_incl) {
    key_range_t::right_bound_t rb;
    rb.key.assign(right_incl);
    rb.unbounded = false;
    bool ok = rb.increment();
    guarantee(ok);
    return rb;
}

continue_bool_t btree_backfill_pre_atoms(
        superblock_t *superblock,
        release_superblock_t release_superblock,
        value_sizer_t *sizer,
        const key_range_t &range,
        repli_timestamp_t since_when,
        btree_backfill_pre_atom_consumer_t *pre_atom_consumer,
        /* RSI(raft): Respect interruptor */
        UNUSED signal_t *interruptor) {
    class callback_t : public depth_first_traversal_callback_t {
    public:
        continue_bool_t filter_range_ts(
                UNUSED const btree_key_t *left_excl_or_null,
                const btree_key_t *right_incl,
                repli_timestamp_t timestamp,
                bool *skip_out) {
            *skip_out = timestamp <= since_when;
            if (*skip_out) {
                return pre_atom_consumer->on_empty_range(
                    convert_right_bound(right_incl));
            } else {
                return continue_bool_t::CONTINUE;
            }
        }

        continue_bool_t handle_pre_leaf(
                const counted_t<counted_buf_lock_t> &buf_lock,
                const counted_t<counted_buf_read_t> &buf_read,
                const btree_key_t *left_excl_or_null,
                const btree_key_t *right_incl,
                bool *skip_out) {
            *skip_out = true;
            const leaf_node_t *lnode = static_cast<const leaf_node_t *>(
                buf_read->get_data_read());
            repli_timestamp_t min_deletion_timestamp =
                leaf::min_deletion_timestamp(sizer, lnode, buf_lock->get_recency());
            if (min_deletion_timestamp > since_when) {
                /* We might be missing deletion entries, so re-transmit the entire node
                */
                backfill_pre_atom_t pre_atom;
                pre_atom.range = key_range_t(
                    left_excl_or_null == nullptr
                        ? key_range_t::bound_t::none : key_range_t::bound_t::open,
                    left_excl_or_null,
                    key_range_t::bound_t::closed,
                    right_incl);
                return pre_atom_consumer->on_pre_atom(std::move(pre_atom));
            } else {
                std::vector<const btree_key_t *> keys;
                leaf::visit_entries(
                    sizer, lnode, buf_lock->get_recency(),
                    [&](const btree_key_t *key, repli_timestamp_t timestamp,
                            const void *) -> bool {
                        if ((left_excl_or_null != nullptr &&
                                    btree_key_cmp(key, left_excl_or_null) != 1)
                                || btree_key_cmp(key, right_incl) == 1) {
                            return continue_bool_t::CONTINUE;
                        }
                        if (timestamp <= since_when) {
                            return continue_bool_t::ABORT;
                        }
                        keys.push_back(key);
                        return continue_bool_t::CONTINUE;
                    });
                std::sort(keys.begin(), keys.end(),
                    [](const btree_key_t *k1, const btree_key_t *k2) {
                        return btree_key_cmp(k1, k2) == -1;
                    });
                for (const btree_key_t *key : keys) {
                    backfill_pre_atom_t pre_atom;
                    pre_atom.range = key_range_t(key);
                    if (continue_bool_t::ABORT ==
                            pre_atom_consumer->on_pre_atom(std::move(pre_atom))) {
                        return continue_bool_t::ABORT;
                    }
                }
                return pre_atom_consumer->on_empty_range(
                    convert_right_bound(right_incl));
            }
        }

        continue_bool_t handle_pair(scoped_key_value_t &&) {
            unreachable();
        }

        btree_backfill_pre_atom_consumer_t *pre_atom_consumer;
        key_range_t range;
        repli_timestamp_t since_when;
        value_sizer_t *sizer;
    } callback;
    callback.pre_atom_consumer = pre_atom_consumer;
    callback.range = range;
    callback.since_when = since_when;
    callback.sizer = sizer;
    return btree_depth_first_traversal(
        superblock,
        range,
        &callback,
        FORWARD,
        release_superblock);
}

/* The `backfill_atom_loader_t` gets backfill atoms from the `backfill_atom_preparer_t`,
but that the actual row values have not been loaded into the atoms yet. It loads the
values from the cache and then passes the atoms on to the `backfill_atom_consumer_t`. */
class backfill_atom_loader_t {
public:
    backfill_atom_loader_t(
            btree_backfill_atom_consumer_t *_consumer, cond_t *_abort_cond) :
        consumer(_consumer), abort_cond(_abort_cond) { }

    /* `on_atom()` and `on_empty_range()` will be called in lexicographical order. They
    will always be called from the same coroutine, so if a call blocks the traversal will
    be paused until it returns. */

    /* The atom passed to `on_atom` is complete except for the `value` field of each
    pair. If the pair has a value, then instead of containing that value, `pair->value`
    will contain a pointer into `buf_read.get_data_read()` which can be used to actually
    load the value. The pointer is stored in the `std::vector<char>` that would normally
    be supposed to store the value. This is kind of a hack, but it will work and it's
    localized to these two types. */
    void on_atom(
            backfill_atom_t &&atom,
            const counted_t<counted_buf_lock_t> &buf_lock,
            const counted_t<counted_buf_read_t> &buf_read) {
        new_semaphore_acq_t sem_acq(&semaphore, atom.pairs.size());
        cond_t non_interruptor;   /* RSI(raft): figure out interruption */
        wait_interruptible(sem_acq.acquisition_signal(), &non_interruptor);
        coro_t::spawn_sometime(std::bind(
            &backfill_atom_loader_t::handle_atom, this,
            std::move(atom), buf_lock, buf_read, std::move(sem_acq),
            fifo_source.enter_write(), drainer.lock()));
    }

    void on_empty_range(const btree_key_t *right_incl) {
        new_semaphore_acq_t sem_acq(&semaphore, 1);
        cond_t non_interruptor;   /* RSI(raft): figure out interruption */
        wait_interruptible(sem_acq.acquisition_signal(), &non_interruptor);
        coro_t::spawn_sometime(std::bind(
            &backfill_atom_loader_t::handle_empty_range, this,
            threshold, std::move(sem_acq), fifo_source.enter_write(), drainer.lock()));
    }

    void finish(signal_t *interruptor) {
        fifo_enforcer_sink_t::exit_write_t exit_write(
            &fifo_sink, fifo_source.enter_write());
        wait_interruptible(&exit_write, interruptor);
    }

private:
    void handle_atom(
            backfill_atom_t &&atom,
            const counted_t<counted_buf_lock_t> &buf_lock,
            const counted_t<counted_buf_read_t> &buf_read,
            new_semaphore_acq_t &&,
            fifo_enforcer_write_token_t token,
            auto_drainer_t::lock_t keepalive) {
        try {
            pmap(atom.pairs.begin(), atom.pairs.end(), [&](backfill_atom_t::pair_t &p) {
                try {
                    if (!static_cast<bool>(p.value)) {
                        /* It's a deletion; we don't need to load anything. */
                        return;
                    }
                    rassert(p.value->size() == sizeof(void *));
                    void *value_ptr = *reinterpret_cast<void *const *>(p.value->data());
                    p.value->clear();
                    atom_consumer->copy_value(buf_lock.get(), value_ptr,
                        keepalive.get_drain_signal(), &*p.value);
                } catch (const interrupted_exc_t &) {
                    /* we'll check this outside the `pmap()` */
                }
            });
            if (keepalive.get_drain_signal()->is_pulsed()) {
                throw interrupted_exc_t();
            }
            fifo_enforcer_sink_t::exit_write_t exit_write(&fifo_sink, token);
            wait_interruptible(&exit_write, keepalive.get_drain_signal());
            if (abort_signal->is_pulsed()) {
                return;
            }
            if (continue_bool_t::ABORT == atom_consumer->on_atom(std::move(atom))) {
                abort_signal->pulse();
            }
        } catch (const interrupted_exc_t &) {
            /* ignore */
        }
    }

    void handle_empty_range(
            const key_range_t::right_bound_t &threshold,
            new_semaphore_acq_t &&,
            fifo_enforcer_write_token_t token,
            auto_drainer_t::lock_t keepalive) {
        try {
            fifo_enforcer_sink_t::exit_write_t exit_write(&fifo_sink, token);
            wait_interruptible(&exit_write, keepalive.get_drain_signal());
            if (abort_signal->is_pulsed()) {
                return;
            }
            if (continue_bool_t::ABORT == atom_consumer->on_empty_range(threshold)) {
                abort_signal->pulse();
            }
        } catch (const interrupted_exc_t &) {
            /* ignore */
        }
    }

    btree_backfill_atom_consumer_t *atom_consumer;
    cond_t *abort_cond;
    new_semaphore_t semaphore;
    fifo_enforcer_source_t fifo_source;
    fifo_enforcer_sink_t fifo_sink;
    auto_drainer_t drainer;
};

/* `backfill_atom_preparer_t` visits leaf nodes using callbacks from the
`btree_depth_first_traversal()`. At each leaf node, it constructs a series of
`backfill_atom_t`s describing the leaf, but doesn't set their values yet; in place of the
values, it stores a pointer to where the value can be loaded from the leaf. Then it
passes them to the `backfill_atom_loader_t` to do the actual loading. */
class backfill_atom_preparer_t : public depth_first_traversal_callback_t {
public:
    backfill_atom_preparer_t(
            value_sizer_t *_sizer,
            btree_backfill_pre_atom_producer_t *_pre_atom_producer,
            signal_t *_abort_cond,
            backfill_atom_loader_t *_loader) :
        sizer(_sizer), pre_atom_producer(_pre_atom_producer), abort_cond(_abort_cond),
        loader(_loader) { }

private:
    /* If `abort_cond` is pulsed we want to abort the traversal. The other methods use
    `return get_continue()` as a way to say "continue the traversal unless `abort_cond`
    is pulsed". */
    continue_bool_t get_continue() {
        return abort_cond.is_pulsed()
            ? continue_bool_t::ABORT : continue_bool_t::CONTINUE;
    }

    continue_bool_t filter_range_ts(
            const btree_key_t *left_excl_or_null,
            const btree_key_t *right_incl,
            repli_timestamp_t timestamp,
            bool *skip_out) {
        bool has_pre_atoms;
        if (continue_bool_t::ABORT == pre_atom_producer->peek_range(
                left_excl_or_null, right_incl, &has_pre_atoms)) {
            return continue_bool_t::ABORT;
        }
        *skip_out = timestamp <= since_when && !has_pre_atoms;
        if (*skip_out) {
            loader->on_empty_range(right_incl);
            /* There are no pre atoms in the range, but we need to call `consume()`
            anyway so that our calls to the `pre_atom_producer` are consecutive. */
            continue_bool_t cont = pre_atom_producer->consume_range(
                left_excl_or_null, right_incl,
                [](const backfill_pre_atom_t &) { unreachable(); });
            if (cont == continue_bool_t::ABORT) {
                return continue_bool_t::ABORT;
            }
        }
        return get_continue();
    }

    continue_bool_t handle_pre_leaf(
            const counted_t<counted_buf_lock_t> &buf_lock,
            const counted_t<counted_buf_read_t> &buf_read,
            const btree_key_t *left_excl_or_null,
            const btree_key_t *right_incl,
            bool *skip_out) {
        *skip_out = false;
        key_range_t leaf_range(
            left_excl_or_null == nullptr
                ? key_range_t::bound_t::none : key_range_t::bound_t::open,
            left_excl_or_null,
            key_range_t::bound_t::closed,
            right_incl);
        const leaf_node_t *lnode = static_cast<const leaf_node_t *>(
            buf_read->get_data_read());

        repli_timestamp_t cutoff =
            leaf::deletion_cutoff_timestamp(sizer, lnode, buf_lock->get_recency());
        if (cutoff > since_when) {
            /* We might be missing deletion entries, so re-transmit the entire node as a
            single `backfill_atom_t` */
            backfill_atom_t atom;
            atom.deletion_cutoff_timestamp = cutoff;
            atom.range = leaf_range;
            loader->on_atom(std::move(atom), buf_lock, buf_read);

            /* We're not going to use these pre atoms, but we need to call `consume()`
            anyway so that our calls to the `pre_atom_producer` are consecutive. */
            continue_bool_t cont =
                pre_atom_producer->consume_range(left_excl_or_null, right_incl,
                    [](const backfill_pre_atom_t &) {} );
            if (cont == continue_bool_t::ABORT) {
                return continue_bool_t::ABORT;
            }

            return get_continue();

        } else {
            /* For each pre atom, make a backfill atom (which is initially empty) */
            std::list<backfill_atom_t> atoms_from_pre;
            continue_bool_t cont =
                pre_atom_producer->consume_range(left_excl_or_null, right_incl,
                    [&](const backfill_pre_atom_t &pre_atom) {
                        backfill_atom_t atom;
                        atom.range = pre_atom.range.intersection(leaf_range);
                        atom.min_deletion_timestamp = min_deletion_timestamp;
                        atoms_from_pre.push_back(atom);
                    });
            if (cont == continue_bool_t::ABORT) {
                return continue_bool_t::ABORT;
            }

            /* Find each key-value pair or deletion entry that falls within the range of
            a pre atom or that changed since `since_when`. If it falls within the range
            of a pre atom, put it into the corresponding atom in `atoms_from_pre`;
            otherwise, make a new atom for it in `atoms_from_time`. */
            std::list<backfill_atom_t> atoms_from_time;
            leaf::visit_entries(
                sizer, lnode, buf_lock->get_recency(),
                [&](const btree_key_t *key, repli_timestamp_t timestamp,
                        const void *value_or_null) -> bool {
                    /* The leaf node might be partially outside the range of the
                    backfill, so we might have to skip some keys */
                    if (!leaf_range.contains_key(key)) {
                        return continue_bool_t::CONTINUE;
                    }

                    /* In the most common case, `atoms_from_pre` is totally empty. Since
                    we ignore entries that are older than `since_when` unless they are in
                    a pre atom, we can optimize things slightly by aborting the leaf node
                    iteration early. */
                    if (timestamp <= since_when && atoms_from_pre.empty()) {
                        return continue_bool_t::ABORT;
                    }

                    /* We'll set `atom` to the `backfill_atom_t` where this key-value
                    pair should be inserted. First we check if there's an atom in
                    `atoms_from_pre` that contains the key. If not, we'll create a new
                    atom in `atoms_from_time`. */
                    backfill_atom_t *atom = nullptr;
                    /* Linear search sucks, but there probably won't be a whole lot of
                    pre atoms, so it's OK for now. */
                    for (backfill_atom_t &a : atoms_from_pre) {
                        if (a.range.contains_key(key)) {
                            atom = &a;
                            break;
                        }
                    }
                    if (atom != nullptr) {
                        /* We didn't find an atom in `atoms_from_pre`. */
                        if (timestamp > since_when) {
                            /* We should create a new atom for this key-value pair */
                            atoms_from_time.push_back(backfill_atom_t());
                            atom = &atoms_from_time.back();
                            atom->range = key_range_t(key);
                            atom->min_deletion_timesstamp =
                                repli_timestamp_t::distant_past;
                        } else {
                            /* Ignore this key-value pair */
                            return continue_bool_t::CONTINUE;
                        }
                    }

                    rassert(atom->range.contains_key(key));
                    rassert(timestamp >= atom->min_deletion_timestamp);

                    size_t i = atom->pairs.size();
                    atom->pairs.resize(i + 1);
                    atom->pairs[i].key.assign(key);
                    atom->pairs[i].recency = timestamp;
                    if (value_or_null != nullptr) {
                        /* Store `value_or_null` in the `value` field as a sequence of
                        8 (or 4 or whatever) `char`s describing its actual pointer value.
                        */
                        atom->pairs[i].value.emplace(
                            reinterpret_cast<const char *>(&value_or_null),
                            reinterpret_cast<const char *>(1 + &value_or_null));
                    }
                    return continue_bool_t::CONTINUE;
                });

            /* `leaf::visit_entries` doesn't necessarily go in lexicographical order. So
            `atoms_from_time` is currently unsorted and we need to sort it. */
            atoms_from_time.sort(
                [](const backfill_atom_t &a1, const backfill_atom_t &a2) {
                    return a1.range.left < a2.range.left;
                });

            /* Merge `atoms_from_time` into `atoms_from_pre`, preserving order. */
            atoms_from_pre.merge(
                std::move(atoms_from_time),
                [](const backfill_atom_t &a1, const backfill_atom_t &a2) {
                    rassert(!a1.range.overlaps(a2.range));
                    return a1.range.left < a2.range.left;
                });

            /* Send the results to the loader */
            for (backfill_atom_t &&a : atoms_from_pre) {
                loader->on_atom(std::move(a), buf_lock, buf_read);
            }
            loader->on_empty_range(right_incl);

            return get_continue();
        }
    }

    value_sizer_t *sizer;
    btree_backfill_pre_atom_producer_t *pre_atom_producer;
    signal_t *abort_cond;
    backfill_atom_loader_t *loader;
};

continue_bool_t btree_backfill_atoms(
        superblock_t *superblock,
        release_superblock_t release_superblock,
        value_sizer_t *sizer,
        const key_range_t &range,
        repli_timestamp_t since_when,
        btree_backfill_pre_atom_producer_t *pre_atom_producer,
        btree_backfill_atom_consumer_t *atom_consumer,
        signal_t *interruptor) {
    cond_t abort_cond;
    backfill_atom_loader_t loader(atom_consumer, &abort_cond);
    backfill_atom_preparer_t preparer(sizer, pre_atom_producer, &abort_cond, &loader);
    if (continue_bool_t::ABORT == btree_concurrent_traversal(
            superblock, range, &preparer, FORWARD, release_superblock)) {
        return continue_bool_t::ABORT;
    }
    loader.finish(interruptor);
    return continue_bool_t::CONTINUE;
}

