// Copyright 2010-2013 RethinkDB, all rights reserved.
#include "errors.hpp"

#include "btree/node.hpp"
#include "btree/operations.hpp"
#include "btree/secondary_operations.hpp"
#include "btree/slice.hpp"
#include "concurrency/cond_var.hpp"
#include "repli_timestamp.hpp"

// Run backfilling at a reduced priority
#define BACKFILL_CACHE_PRIORITY 10

using alt::alt_access_t;
using alt::alt_buf_lock_t;
using alt::alt_buf_parent_t;
using alt::alt_cache_t;
using alt::alt_create_t;
using alt::alt_txn_t;

void btree_slice_t::create(alt_cache_t *cache,
                           const std::vector<char> &metainfo_key,
                           const std::vector<char> &metainfo_value) {

    alt_txn_t txn(cache, write_durability_t::HARD, repli_timestamp_t::distant_past, 1);

    create(SUPERBLOCK_ID, alt_buf_parent_t(&txn), metainfo_key, metainfo_value);
}

void btree_slice_t::create(block_id_t superblock_id,
                           alt_buf_parent_t parent,
                           const std::vector<char> &metainfo_key,
                           const std::vector<char> &metainfo_value) {
    // The superblock was already created by cache_t::create or by creating it and
    // getting the block id.
    alt_buf_lock_t superblock(parent, superblock_id, alt_access_t::write);

    alt::alt_buf_write_t sb_write(&superblock);
    auto sb = static_cast<btree_superblock_t *>(sb_write.get_data_write());
    bzero(sb, parent.cache()->get_block_size().value());

    // sb->metainfo_blob has been properly zeroed.
    sb->magic = btree_superblock_t::expected_magic;
    sb->root_block = NULL_BLOCK_ID;
    sb->stat_block = NULL_BLOCK_ID;
    sb->sindex_block = NULL_BLOCK_ID;

    set_superblock_metainfo(&superblock, metainfo_key, metainfo_value);

    alt::alt_buf_lock_t sindex_block(&superblock, alt_create_t::create);
    initialize_secondary_indexes(&sindex_block);
    sb->sindex_block = sindex_block.get_block_id();
}

btree_slice_t::btree_slice_t(alt_cache_t *c, perfmon_collection_t *parent,
                             const std::string &identifier,
                             block_id_t _superblock_id)
    : stats(parent, identifier),
      cache_(c),
      superblock_id_(_superblock_id) {
    cache()->create_cache_account(BACKFILL_CACHE_PRIORITY, &backfill_account);

    pre_begin_txn_checkpoint_.set_tagappend("pre_begin_txn");
}

btree_slice_t::~btree_slice_t() { }
