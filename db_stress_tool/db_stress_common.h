//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// The test uses an array to compare against values written to the database.
// Keys written to the array are in 1:1 correspondence to the actual values in
// the database according to the formula in the function GenerateValue.

// Space is reserved in the array from 0 to FLAGS_max_key and values are
// randomly written/deleted/read from those positions. During verification we
// compare all the positions in the array. To shorten/elongate the running
// time, you could change the settings: FLAGS_max_key, FLAGS_ops_per_thread,
// (sometimes also FLAGS_threads).
//
// NOTE that if FLAGS_test_batches_snapshots is set, the test will have
// different behavior. See comment of the flag for details.

#ifdef GFLAGS
#pragma once
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <exception>
#include <queue>
#include <thread>

#include "db/db_impl/db_impl.h"
#include "db/version_set.h"
#include "db_stress_tool/db_stress_env_wrapper.h"
#include "db_stress_tool/db_stress_listener.h"
#include "db_stress_tool/db_stress_shared_state.h"
#include "db_stress_tool/db_stress_test_base.h"
#include "hdfs/env_hdfs.h"
#include "logging/logging.h"
#include "monitoring/histogram.h"
#include "options/options_helper.h"
#include "port/port.h"
#include "rocksdb/cache.h"
#include "rocksdb/env.h"
#include "rocksdb/slice.h"
#include "rocksdb/slice_transform.h"
#include "rocksdb/statistics.h"
#include "rocksdb/utilities/backupable_db.h"
#include "rocksdb/utilities/checkpoint.h"
#include "rocksdb/utilities/db_ttl.h"
#include "rocksdb/utilities/debug.h"
#include "rocksdb/utilities/options_util.h"
#include "rocksdb/utilities/transaction.h"
#include "rocksdb/utilities/transaction_db.h"
#include "rocksdb/write_batch.h"
#include "util/coding.h"
#include "util/compression.h"
#include "util/crc32c.h"
#include "util/gflags_compat.h"
#include "util/mutexlock.h"
#include "util/random.h"
#include "util/string_util.h"
// SyncPoint is not supported in Released Windows Mode.
#if !(defined NDEBUG) || !defined(OS_WIN)
#include "test_util/sync_point.h"
#endif  // !(defined NDEBUG) || !defined(OS_WIN)
#include "test_util/testutil.h"

#include "utilities/merge_operators.h"

using GFLAGS_NAMESPACE::ParseCommandLineFlags;
using GFLAGS_NAMESPACE::RegisterFlagValidator;
using GFLAGS_NAMESPACE::SetUsageMessage;

DECLARE_uint64(seed);
DECLARE_bool(read_only);
DECLARE_int64(max_key);
DECLARE_double(hot_key_alpha);
DECLARE_int32(column_families);
DECLARE_string(options_file);
DECLARE_int64(active_width);
DECLARE_bool(test_batches_snapshots);
DECLARE_bool(atomic_flush);
DECLARE_bool(test_cf_consistency);
DECLARE_int32(threads);
DECLARE_int32(ttl);
DECLARE_int32(value_size_mult);
DECLARE_int32(compaction_readahead_size);
DECLARE_bool(enable_pipelined_write);
DECLARE_bool(verify_before_write);
DECLARE_bool(histogram);
DECLARE_bool(destroy_db_initially);
DECLARE_bool(verbose);
DECLARE_bool(progress_reports);
DECLARE_uint64(db_write_buffer_size);
DECLARE_int32(write_buffer_size);
DECLARE_int32(max_write_buffer_number);
DECLARE_int32(min_write_buffer_number_to_merge);
DECLARE_int32(max_write_buffer_number_to_maintain);
DECLARE_int64(max_write_buffer_size_to_maintain);
DECLARE_double(memtable_prefix_bloom_size_ratio);
DECLARE_bool(memtable_whole_key_filtering);
DECLARE_int32(open_files);
DECLARE_int64(compressed_cache_size);
DECLARE_int32(compaction_style);
DECLARE_int32(level0_file_num_compaction_trigger);
DECLARE_int32(level0_slowdown_writes_trigger);
DECLARE_int32(level0_stop_writes_trigger);
DECLARE_int32(block_size);
DECLARE_int32(format_version);
DECLARE_int32(index_block_restart_interval);
DECLARE_int32(max_background_compactions);
DECLARE_int32(num_bottom_pri_threads);
DECLARE_int32(compaction_thread_pool_adjust_interval);
DECLARE_int32(compaction_thread_pool_variations);
DECLARE_int32(max_background_flushes);
DECLARE_int32(universal_size_ratio);
DECLARE_int32(universal_min_merge_width);
DECLARE_int32(universal_max_merge_width);
DECLARE_int32(universal_max_size_amplification_percent);
DECLARE_int32(clear_column_family_one_in);
DECLARE_int32(set_options_one_in);
DECLARE_int32(set_in_place_one_in);
DECLARE_int64(cache_size);
DECLARE_bool(cache_index_and_filter_blocks);
DECLARE_bool(use_clock_cache);
DECLARE_uint64(subcompactions);
DECLARE_uint64(periodic_compaction_seconds);
DECLARE_uint64(compaction_ttl);
DECLARE_bool(allow_concurrent_memtable_write);
DECLARE_bool(enable_write_thread_adaptive_yield);
DECLARE_int32(reopen);
DECLARE_double(bloom_bits);
DECLARE_bool(use_block_based_filter);
DECLARE_bool(partition_filters);
DECLARE_int32(index_type);
DECLARE_string(db);
DECLARE_string(secondaries_base);
DECLARE_bool(enable_secondary);
DECLARE_string(expected_values_path);
DECLARE_bool(verify_checksum);
DECLARE_bool(mmap_read);
DECLARE_bool(mmap_write);
DECLARE_bool(use_direct_reads);
DECLARE_bool(use_direct_io_for_flush_and_compaction);
DECLARE_bool(statistics);
DECLARE_bool(sync);
DECLARE_bool(use_fsync);
DECLARE_int32(kill_random_test);
DECLARE_string(kill_prefix_blacklist);
DECLARE_bool(disable_wal);
DECLARE_uint64(recycle_log_file_num);
DECLARE_int64(target_file_size_base);
DECLARE_int32(target_file_size_multiplier);
DECLARE_uint64(max_bytes_for_level_base);
DECLARE_double(max_bytes_for_level_multiplier);
DECLARE_int32(range_deletion_width);
DECLARE_uint64(rate_limiter_bytes_per_sec);
DECLARE_bool(rate_limit_bg_reads);
DECLARE_bool(use_txn);
DECLARE_uint64(txn_write_policy);
DECLARE_bool(unordered_write);
DECLARE_int32(backup_one_in);
DECLARE_int32(checkpoint_one_in);
DECLARE_int32(ingest_external_file_one_in);
DECLARE_int32(ingest_external_file_width);
DECLARE_int32(compact_files_one_in);
DECLARE_int32(compact_range_one_in);
DECLARE_int32(flush_one_in);
DECLARE_int32(pause_background_one_in);
DECLARE_int32(compact_range_width);
DECLARE_int32(acquire_snapshot_one_in);
DECLARE_bool(compare_full_db_state_snapshot);
DECLARE_uint64(snapshot_hold_ops);
DECLARE_bool(long_running_snapshots);
DECLARE_bool(use_multiget);
DECLARE_int32(readpercent);
DECLARE_int32(prefixpercent);
DECLARE_int32(writepercent);
DECLARE_int32(delpercent);
DECLARE_int32(delrangepercent);
DECLARE_int32(nooverwritepercent);
DECLARE_int32(iterpercent);
DECLARE_uint64(num_iterations);
DECLARE_string(compression_type);
DECLARE_int32(compression_max_dict_bytes);
DECLARE_int32(compression_zstd_max_train_bytes);
DECLARE_string(checksum_type);
DECLARE_string(hdfs);
DECLARE_string(env_uri);
DECLARE_uint64(ops_per_thread);
DECLARE_uint64(log2_keys_per_lock);
DECLARE_uint64(max_manifest_file_size);
DECLARE_bool(in_place_update);
DECLARE_int32(secondary_catch_up_one_in);
DECLARE_string(memtablerep);
DECLARE_int32(prefix_size);
DECLARE_bool(use_merge);
DECLARE_bool(use_full_merge_v1);
DECLARE_int32(sync_wal_one_in);
DECLARE_bool(avoid_unnecessary_blocking_io);
DECLARE_bool(write_dbid_to_manifest);
DECLARE_uint64(max_write_batch_group_size_bytes);
DECLARE_bool(level_compaction_dynamic_level_bytes);
DECLARE_int32(verify_checksum_one_in);

const long KB = 1024;
const int kRandomValueMaxFactor = 3;
const int kValueMaxLen = 100;

// wrapped posix or hdfs environment
extern rocksdb::DbStressEnvWrapper* FLAGS_env;

extern enum rocksdb::CompressionType FLAGS_compression_type_e;
extern enum rocksdb::ChecksumType FLAGS_checksum_type_e;

enum RepFactory { kSkipList, kHashSkipList, kVectorRep };

inline enum RepFactory StringToRepFactory(const char* ctype) {
  assert(ctype);

  if (!strcasecmp(ctype, "skip_list"))
    return kSkipList;
  else if (!strcasecmp(ctype, "prefix_hash"))
    return kHashSkipList;
  else if (!strcasecmp(ctype, "vector"))
    return kVectorRep;

  fprintf(stdout, "Cannot parse memreptable %s\n", ctype);
  return kSkipList;
}

extern enum RepFactory FLAGS_rep_factory;

namespace rocksdb {
inline enum rocksdb::CompressionType StringToCompressionType(
    const char* ctype) {
  assert(ctype);

  if (!strcasecmp(ctype, "none"))
    return rocksdb::kNoCompression;
  else if (!strcasecmp(ctype, "snappy"))
    return rocksdb::kSnappyCompression;
  else if (!strcasecmp(ctype, "zlib"))
    return rocksdb::kZlibCompression;
  else if (!strcasecmp(ctype, "bzip2"))
    return rocksdb::kBZip2Compression;
  else if (!strcasecmp(ctype, "lz4"))
    return rocksdb::kLZ4Compression;
  else if (!strcasecmp(ctype, "lz4hc"))
    return rocksdb::kLZ4HCCompression;
  else if (!strcasecmp(ctype, "xpress"))
    return rocksdb::kXpressCompression;
  else if (!strcasecmp(ctype, "zstd"))
    return rocksdb::kZSTD;

  fprintf(stderr, "Cannot parse compression type '%s'\n", ctype);
  return rocksdb::kSnappyCompression;  // default value
}

inline enum rocksdb::ChecksumType StringToChecksumType(const char* ctype) {
  assert(ctype);
  auto iter = rocksdb::checksum_type_string_map.find(ctype);
  if (iter != rocksdb::checksum_type_string_map.end()) {
    return iter->second;
  }
  fprintf(stderr, "Cannot parse checksum type '%s'\n", ctype);
  return rocksdb::kCRC32c;
}

inline std::string ChecksumTypeToString(rocksdb::ChecksumType ctype) {
  auto iter = std::find_if(
      rocksdb::checksum_type_string_map.begin(),
      rocksdb::checksum_type_string_map.end(),
      [&](const std::pair<std::string, rocksdb::ChecksumType>&
              name_and_enum_val) { return name_and_enum_val.second == ctype; });
  assert(iter != rocksdb::checksum_type_string_map.end());
  return iter->first;
}

inline std::vector<std::string> SplitString(std::string src) {
  std::vector<std::string> ret;
  if (src.empty()) {
    return ret;
  }
  size_t pos = 0;
  size_t pos_comma;
  while ((pos_comma = src.find(',', pos)) != std::string::npos) {
    ret.push_back(src.substr(pos, pos_comma - pos));
    pos = pos_comma + 1;
  }
  ret.push_back(src.substr(pos, src.length()));
  return ret;
}

#ifdef _MSC_VER
#pragma warning(push)
// truncation of constant value on static_cast
#pragma warning(disable : 4309)
#endif
inline bool GetNextPrefix(const rocksdb::Slice& src, std::string* v) {
  std::string ret = src.ToString();
  for (int i = static_cast<int>(ret.size()) - 1; i >= 0; i--) {
    if (ret[i] != static_cast<char>(255)) {
      ret[i] = ret[i] + 1;
      break;
    } else if (i != 0) {
      ret[i] = 0;
    } else {
      // all FF. No next prefix
      return false;
    }
  }
  *v = ret;
  return true;
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// convert long to a big-endian slice key
extern inline std::string Key(int64_t val) {
  std::string little_endian_key;
  std::string big_endian_key;
  PutFixed64(&little_endian_key, val);
  assert(little_endian_key.size() == sizeof(val));
  big_endian_key.resize(sizeof(val));
  for (size_t i = 0; i < sizeof(val); ++i) {
    big_endian_key[i] = little_endian_key[sizeof(val) - 1 - i];
  }
  return big_endian_key;
}

extern inline bool GetIntVal(std::string big_endian_key, uint64_t* key_p) {
  unsigned int size_key = sizeof(*key_p);
  assert(big_endian_key.size() == size_key);
  std::string little_endian_key;
  little_endian_key.resize(size_key);
  for (size_t i = 0; i < size_key; ++i) {
    little_endian_key[i] = big_endian_key[size_key - 1 - i];
  }
  Slice little_endian_slice = Slice(little_endian_key);
  return GetFixed64(&little_endian_slice, key_p);
}

extern inline std::string StringToHex(const std::string& str) {
  std::string result = "0x";
  result.append(Slice(str).ToString(true));
  return result;
}

// Unified output format for double parameters
extern inline std::string FormatDoubleParam(double param) {
  return std::to_string(param);
}

// Make sure that double parameter is a value we can reproduce by
// re-inputting the value printed.
extern inline void SanitizeDoubleParam(double* param) {
  *param = std::atof(FormatDoubleParam(*param).c_str());
}

extern void PoolSizeChangeThread(void* v);

extern void PrintKeyValue(int cf, uint64_t key, const char* value, size_t sz);

extern int64_t GenerateOneKey(ThreadState* thread, uint64_t iteration);

extern std::vector<int64_t> GenerateNKeys(ThreadState* thread, int num_keys,
                                          uint64_t iteration);

extern size_t GenerateValue(uint32_t rand, char* v, size_t max_sz);

extern StressTest* CreateCfConsistencyStressTest();
extern StressTest* CreateBatchedOpsStressTest();
extern StressTest* CreateNonBatchedOpsStressTest();
extern void InitializeHotKeyGenerator(double alpha);
extern int64_t GetOneHotKeyID(double rand_seed, int64_t max_key);
}  // namespace rocksdb
#endif  // GFLAGS
