// Copyright (C) 2023 Speedb Ltd. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Various APIs for configuring, creating, and monitoring read caches.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "rocksdb/compression_type.h"
#include "rocksdb/data_structure.h"
#include "rocksdb/memory_allocator.h"

namespace ROCKSDB_NAMESPACE {

class Cache;  // defined in advanced_cache.h
struct ConfigOptions;
class SecondaryCache;

// Classifications of block cache entries.
//
// Developer notes: Adding a new enum to this class requires corresponding
// updates to `kCacheEntryRoleToCamelString` and
// `kCacheEntryRoleToHyphenString`. Do not add to this enum after `kMisc` since
// `kNumCacheEntryRoles` assumes `kMisc` comes last.
enum class CacheEntryRole {
  // Block-based table data block
  kDataBlock,
  // Block-based table filter block (full or partitioned)
  kFilterBlock,
  // Block-based table metadata block for partitioned filter
  kFilterMetaBlock,
  // OBSOLETE / DEPRECATED: old/removed block-based filter
  kDeprecatedFilterBlock,
  // Block-based table index block
  kIndexBlock,
  // Other kinds of block-based table block
  kOtherBlock,
  // WriteBufferManager's charge to account for its memtable usage
  kWriteBuffer,
  // Compression dictionary building buffer's charge to account for
  // its memory usage
  kCompressionDictionaryBuildingBuffer,
  // Filter's charge to account for
  // (new) bloom and ribbon filter construction's memory usage
  kFilterConstruction,
  // BlockBasedTableReader's charge to account for its memory usage
  kBlockBasedTableReader,
  // FileMetadata's charge to account for its memory usage
  kFileMetadata,
  // Blob value (when using the same cache as block cache and blob cache)
  kBlobValue,
  // Blob cache's charge to account for its memory usage (when using a
  // separate block cache and blob cache)
  kBlobCache,
  // Default bucket, for miscellaneous cache entries. Do not use for
  // entries that could potentially add up to large usage.
  kMisc,
};
constexpr uint32_t kNumCacheEntryRoles =
    static_cast<uint32_t>(CacheEntryRole::kMisc) + 1;

// Obtain a hyphen-separated, lowercase name of a `CacheEntryRole`.
const std::string& GetCacheEntryRoleName(CacheEntryRole);

// A fast bit set for CacheEntryRoles
using CacheEntryRoleSet = SmallEnumSet<CacheEntryRole, CacheEntryRole::kMisc>;

// For use with `GetMapProperty()` for property
// `DB::Properties::kBlockCacheEntryStats`. On success, the map will
// be populated with all keys that can be obtained from these functions.
struct BlockCacheEntryStatsMapKeys {
  static const std::string& CacheId();
  static const std::string& CacheCapacityBytes();
  static const std::string& LastCollectionDurationSeconds();
  static const std::string& LastCollectionAgeSeconds();

  static std::string EntryCount(CacheEntryRole);
  static std::string UsedBytes(CacheEntryRole);
  static std::string UsedPercent(CacheEntryRole);
};

// For use with `GetMapProperty()` for property
// `DB::Properties::kBlockCacheCfStats` and
// 'DB::Properties::kFastBlockCacheCfStats' On success, the map will be
// populated with all keys that can be obtained from these functions.
struct BlockCacheCfStatsMapKeys {
  static const std::string& CfName();
  static const std::string& CacheId();
  static std::string UsedBytes(CacheEntryRole);
};

extern const bool kDefaultToAdaptiveMutex;

enum CacheMetadataChargePolicy {
  // Only the `charge` of each entry inserted into a Cache counts against
  // the `capacity`
  kDontChargeCacheMetadata,
  // In addition to the `charge`, the approximate space overheads in the
  // Cache (in bytes) also count against `capacity`. These space overheads
  // are for supporting fast Lookup and managing the lifetime of entries.
  kFullChargeCacheMetadata
};
const CacheMetadataChargePolicy kDefaultCacheMetadataChargePolicy =
    kFullChargeCacheMetadata;

// Options shared betweeen various cache implementations that
// divide the key space into shards using hashing.
struct ShardedCacheOptions {
  // Capacity of the cache, in the same units as the `charge` of each entry.
  // This is typically measured in bytes, but can be a different unit if using
  // kDontChargeCacheMetadata.
  size_t capacity = 0;

  // Cache is sharded into 2^num_shard_bits shards, by hash of key.
  // If < 0, a good default is chosen based on the capacity and the
  // implementation. (Mutex-based implementations are much more reliant
  // on many shards for parallel scalability.)
  int num_shard_bits = -1;

  // If strict_capacity_limit is set, Insert() will fail if there is not
  // enough capacity for the new entry along with all the existing referenced
  // (pinned) cache entries. (Unreferenced cache entries are evicted as
  // needed, sometimes immediately.) If strict_capacity_limit == false
  // (default), Insert() never fails.
  bool strict_capacity_limit = false;

  // If non-nullptr, RocksDB will use this allocator instead of system
  // allocator when allocating memory for cache blocks.
  //
  // Caveat: when the cache is used as block cache, the memory allocator is
  // ignored when dealing with compression libraries that allocate memory
  // internally (currently only XPRESS).
  std::shared_ptr<MemoryAllocator> memory_allocator;

  // See CacheMetadataChargePolicy
  CacheMetadataChargePolicy metadata_charge_policy =
      kDefaultCacheMetadataChargePolicy;

  // A SecondaryCache instance to use the non-volatile tier.
  std::shared_ptr<SecondaryCache> secondary_cache;

  ShardedCacheOptions() {}
  ShardedCacheOptions(
      size_t _capacity, int _num_shard_bits, bool _strict_capacity_limit,
      std::shared_ptr<MemoryAllocator> _memory_allocator = nullptr,
      CacheMetadataChargePolicy _metadata_charge_policy =
          kDefaultCacheMetadataChargePolicy)
      : capacity(_capacity),
        num_shard_bits(_num_shard_bits),
        strict_capacity_limit(_strict_capacity_limit),
        memory_allocator(std::move(_memory_allocator)),
        metadata_charge_policy(_metadata_charge_policy) {}
};

struct LRUCacheOptions : public ShardedCacheOptions {
  // Ratio of cache reserved for high-priority and low-priority entries,
  // respectively. (See Cache::Priority below more information on the levels.)
  // Valid values are between 0 and 1 (inclusive), and the sum of the two
  // values cannot exceed 1.
  //
  // If high_pri_pool_ratio is greater than zero, a dedicated high-priority LRU
  // list is maintained by the cache. Similarly, if low_pri_pool_ratio is
  // greater than zero, a dedicated low-priority LRU list is maintained.
  // There is also a bottom-priority LRU list, which is always enabled and not
  // explicitly configurable. Entries are spilled over to the next available
  // lower-priority pool if a certain pool's capacity is exceeded.
  //
  // Entries with cache hits are inserted into the highest priority LRU list
  // available regardless of the entry's priority. Entries without hits
  // are inserted into highest priority LRU list available whose priority
  // does not exceed the entry's priority. (For example, high-priority items
  // with no hits are placed in the high-priority pool if available;
  // otherwise, they are placed in the low-priority pool if available;
  // otherwise, they are placed in the bottom-priority pool.) This results
  // in lower-priority entries without hits getting evicted from the cache
  // sooner.
  //
  // Default values: high_pri_pool_ratio = 0.5 (which is referred to as
  // "midpoint insertion"), low_pri_pool_ratio = 0
  double high_pri_pool_ratio = 0.5;
  double low_pri_pool_ratio = 0.0;

  // Whether to use adaptive mutexes for cache shards. Note that adaptive
  // mutexes need to be supported by the platform in order for this to have any
  // effect. The default value is true if RocksDB is compiled with
  // -DROCKSDB_DEFAULT_TO_ADAPTIVE_MUTEX, false otherwise.
  bool use_adaptive_mutex = kDefaultToAdaptiveMutex;

  LRUCacheOptions() {}
  LRUCacheOptions(size_t _capacity, int _num_shard_bits,
                  bool _strict_capacity_limit, double _high_pri_pool_ratio,
                  std::shared_ptr<MemoryAllocator> _memory_allocator = nullptr,
                  bool _use_adaptive_mutex = kDefaultToAdaptiveMutex,
                  CacheMetadataChargePolicy _metadata_charge_policy =
                      kDefaultCacheMetadataChargePolicy,
                  double _low_pri_pool_ratio = 0.0)
      : ShardedCacheOptions(_capacity, _num_shard_bits, _strict_capacity_limit,
                            std::move(_memory_allocator),
                            _metadata_charge_policy),
        high_pri_pool_ratio(_high_pri_pool_ratio),
        low_pri_pool_ratio(_low_pri_pool_ratio),
        use_adaptive_mutex(_use_adaptive_mutex) {}
};

// Create a new cache with a fixed size capacity. The cache is sharded
// to 2^num_shard_bits shards, by hash of the key. The total capacity
// is divided and evenly assigned to each shard. If strict_capacity_limit
// is set, insert to the cache will fail when cache is full. User can also
// set percentage of the cache reserves for high priority entries via
// high_pri_pool_pct.
// num_shard_bits = -1 means it is automatically determined: every shard
// will be at least 512KB and number of shard bits will not exceed 6.
extern std::shared_ptr<Cache> NewLRUCache(
    size_t capacity, int num_shard_bits = -1,
    bool strict_capacity_limit = false, double high_pri_pool_ratio = 0.5,
    std::shared_ptr<MemoryAllocator> memory_allocator = nullptr,
    bool use_adaptive_mutex = kDefaultToAdaptiveMutex,
    CacheMetadataChargePolicy metadata_charge_policy =
        kDefaultCacheMetadataChargePolicy,
    double low_pri_pool_ratio = 0.0);

extern std::shared_ptr<Cache> NewLRUCache(const LRUCacheOptions& cache_opts);

// EXPERIMENTAL
// Options structure for configuring a SecondaryCache instance based on
// LRUCache. The LRUCacheOptions.secondary_cache is not used and
// should not be set.
struct CompressedSecondaryCacheOptions : LRUCacheOptions {
  // The compression method (if any) that is used to compress data.
  CompressionType compression_type = CompressionType::kLZ4Compression;

  // compress_format_version can have two values:
  // compress_format_version == 1 -- decompressed size is not included in the
  // block header.
  // compress_format_version == 2 -- decompressed size is included in the block
  // header in varint32 format.
  uint32_t compress_format_version = 2;

  // Enable the custom split and merge feature, which split the compressed value
  // into chunks so that they may better fit jemalloc bins.
  bool enable_custom_split_merge = false;

  // Kinds of entries that should not be compressed, but can be stored.
  // (Filter blocks are essentially non-compressible but others usually are.)
  CacheEntryRoleSet do_not_compress_roles = {CacheEntryRole::kFilterBlock};

  CompressedSecondaryCacheOptions() {}
  CompressedSecondaryCacheOptions(
      size_t _capacity, int _num_shard_bits, bool _strict_capacity_limit,
      double _high_pri_pool_ratio, double _low_pri_pool_ratio = 0.0,
      std::shared_ptr<MemoryAllocator> _memory_allocator = nullptr,
      bool _use_adaptive_mutex = kDefaultToAdaptiveMutex,
      CacheMetadataChargePolicy _metadata_charge_policy =
          kDefaultCacheMetadataChargePolicy,
      CompressionType _compression_type = CompressionType::kLZ4Compression,
      uint32_t _compress_format_version = 2,
      bool _enable_custom_split_merge = false,
      const CacheEntryRoleSet& _do_not_compress_roles =
          {CacheEntryRole::kFilterBlock})
      : LRUCacheOptions(_capacity, _num_shard_bits, _strict_capacity_limit,
                        _high_pri_pool_ratio, std::move(_memory_allocator),
                        _use_adaptive_mutex, _metadata_charge_policy,
                        _low_pri_pool_ratio),
        compression_type(_compression_type),
        compress_format_version(_compress_format_version),
        enable_custom_split_merge(_enable_custom_split_merge),
        do_not_compress_roles(_do_not_compress_roles) {}
};

// EXPERIMENTAL
// Create a new Secondary Cache that is implemented on top of LRUCache.
extern std::shared_ptr<SecondaryCache> NewCompressedSecondaryCache(
    size_t capacity, int num_shard_bits = -1,
    bool strict_capacity_limit = false, double high_pri_pool_ratio = 0.5,
    double low_pri_pool_ratio = 0.0,
    std::shared_ptr<MemoryAllocator> memory_allocator = nullptr,
    bool use_adaptive_mutex = kDefaultToAdaptiveMutex,
    CacheMetadataChargePolicy metadata_charge_policy =
        kDefaultCacheMetadataChargePolicy,
    CompressionType compression_type = CompressionType::kLZ4Compression,
    uint32_t compress_format_version = 2,
    bool enable_custom_split_merge = false,
    const CacheEntryRoleSet& _do_not_compress_roles = {
        CacheEntryRole::kFilterBlock});

extern std::shared_ptr<SecondaryCache> NewCompressedSecondaryCache(
    const CompressedSecondaryCacheOptions& opts);

// HyperClockCache - A lock-free Cache alternative for RocksDB block cache
// that offers much improved CPU efficiency vs. LRUCache under high parallel
// load or high contention, with some caveats:
// * Not a general Cache implementation: can only be used for
// BlockBasedTableOptions::block_cache, which RocksDB uses in a way that is
// compatible with HyperClockCache.
// * Requires an extra tuning parameter: see estimated_entry_charge below.
// Similarly, substantially changing the capacity with SetCapacity could
// harm efficiency.
// * SecondaryCache is not yet supported.
// * Cache priorities are less aggressively enforced, which could cause
// cache dilution from long range scans (unless they use fill_cache=false).
// * Can be worse for small caches, because if almost all of a cache shard is
// pinned (more likely with non-partitioned filters), then CLOCK eviction
// becomes very CPU intensive.
//
// See internal cache/clock_cache.h for full description.
struct HyperClockCacheOptions : public ShardedCacheOptions {
  // The estimated average `charge` associated with cache entries. This is a
  // critical configuration parameter for good performance from the hyper
  // cache, because having a table size that is fixed at creation time greatly
  // reduces the required synchronization between threads.
  // * If the estimate is substantially too low (e.g. less than half the true
  // average) then metadata space overhead with be substantially higher (e.g.
  // 200 bytes per entry rather than 100). With kFullChargeCacheMetadata, this
  // can slightly reduce cache hit rates, and slightly reduce access times due
  // to the larger working memory size.
  // * If the estimate is substantially too high (e.g. 25% higher than the true
  // average) then there might not be sufficient slots in the hash table for
  // both efficient operation and capacity utilization (hit rate). The hyper
  // cache will evict entries to prevent load factors that could dramatically
  // affect lookup times, instead letting the hit rate suffer by not utilizing
  // the full capacity.
  //
  // A reasonable choice is the larger of block_size and metadata_block_size.
  // When WriteBufferManager (and similar) charge memory usage to the block
  // cache, this can lead to the same effect as estimate being too low, which
  // is better than the opposite. Therefore, the general recommendation is to
  // assume that other memory charged to block cache could be negligible, and
  // ignore it in making the estimate.
  //
  // The best parameter choice based on a cache in use is given by
  // GetUsage() / GetOccupancyCount(), ignoring metadata overheads such as
  // with kDontChargeCacheMetadata. More precisely with
  // kFullChargeCacheMetadata is (GetUsage() - 64 * GetTableAddressCount()) /
  // GetOccupancyCount(). However, when the average value size might vary
  // (e.g. balance between metadata and data blocks in cache), it is better
  // to estimate toward the lower side than the higher side.
  size_t estimated_entry_charge;

  HyperClockCacheOptions(
      size_t _capacity, size_t _estimated_entry_charge,
      int _num_shard_bits = -1, bool _strict_capacity_limit = false,
      std::shared_ptr<MemoryAllocator> _memory_allocator = nullptr,
      CacheMetadataChargePolicy _metadata_charge_policy =
          kDefaultCacheMetadataChargePolicy)
      : ShardedCacheOptions(_capacity, _num_shard_bits, _strict_capacity_limit,
                            std::move(_memory_allocator),
                            _metadata_charge_policy),
        estimated_entry_charge(_estimated_entry_charge) {}

  // Construct an instance of HyperClockCache using these options
  std::shared_ptr<Cache> MakeSharedCache() const;
};

// DEPRECATED - The old Clock Cache implementation had an unresolved bug and
// has been removed. The new HyperClockCache requires an additional
// configuration parameter that is not provided by this API. This function
// simply returns a new LRUCache for functional compatibility.
extern std::shared_ptr<Cache> NewClockCache(
    size_t capacity, int num_shard_bits = -1,
    bool strict_capacity_limit = false,
    CacheMetadataChargePolicy metadata_charge_policy =
        kDefaultCacheMetadataChargePolicy);

}  // namespace ROCKSDB_NAMESPACE
