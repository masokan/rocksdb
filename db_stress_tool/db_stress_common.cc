//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//

#ifdef GFLAGS
#include "db_stress_tool/db_stress_common.h"
#include <cmath>

rocksdb::DbStressEnvWrapper* FLAGS_env = nullptr;
enum rocksdb::CompressionType FLAGS_compression_type_e =
    rocksdb::kSnappyCompression;
enum rocksdb::ChecksumType FLAGS_checksum_type_e = rocksdb::kCRC32c;
enum RepFactory FLAGS_rep_factory = kSkipList;
std::vector<double> sum_probs(100001);
int64_t zipf_sum_size = 100000;

namespace rocksdb {

// Zipfian distribution is generated based on a pre-calculated array.
// It should be used before start the stress test.
// First, the probability distribution function (PDF) of this Zipfian follows
// power low. P(x) = 1/(x^alpha).
// So we calculate the PDF when x is from 0 to zipf_sum_size in first for loop
// and add the PDF value togetger as c. So we get the total probability in c.
// Next, we calculate inverse CDF of Zipfian and store the value of each in
// an array (sum_probs). The rank is from 0 to zipf_sum_size. For example, for
// integer k, its Zipfian CDF value is sum_probs[k].
// Third, when we need to get an integer whose probability follows Zipfian
// distribution, we use a rand_seed [0,1] which follows uniform distribution
// as a seed and search it in the sum_probs via binary search. When we find
// the closest sum_probs[i] of rand_seed, i is the integer that in
// [0, zipf_sum_size] following Zipfian distribution with parameter alpha.
// Finally, we can scale i to [0, max_key] scale.
// In order to avoid that hot keys are close to each other and skew towards 0,
// we use Rando64 to shuffle it.
void InitializeHotKeyGenerator(double alpha) {
  double c = 0;
  for (int64_t i = 1; i <= zipf_sum_size; i++) {
    c = c + (1.0 / std::pow(static_cast<double>(i), alpha));
  }
  c = 1.0 / c;

  sum_probs[0] = 0;
  for (int64_t i = 1; i <= zipf_sum_size; i++) {
    sum_probs[i] =
        sum_probs[i - 1] + c / std::pow(static_cast<double>(i), alpha);
  }
}

// Generate one key that follows the Zipfian distribution. The skewness
// is decided by the parameter alpha. Input is the rand_seed [0,1] and
// the max of the key to be generated. If we directly return tmp_zipf_seed,
// the closer to 0, the higher probability will be. To randomly distribute
// the hot keys in [0, max_key], we use Random64 to shuffle it.
int64_t GetOneHotKeyID(double rand_seed, int64_t max_key) {
  int64_t low = 1, mid, high = zipf_sum_size, zipf = 0;
  while (low <= high) {
    mid = std::floor((low + high) / 2);
    if (sum_probs[mid] >= rand_seed && sum_probs[mid - 1] < rand_seed) {
      zipf = mid;
      break;
    } else if (sum_probs[mid] >= rand_seed) {
      high = mid - 1;
    } else {
      low = mid + 1;
    }
  }
  int64_t tmp_zipf_seed = static_cast<int64_t>(
      std::floor(zipf * max_key / (static_cast<double>(zipf_sum_size))));
  Random64 rand_local(tmp_zipf_seed);
  return rand_local.Next() % max_key;
}

void PoolSizeChangeThread(void* v) {
  assert(FLAGS_compaction_thread_pool_adjust_interval > 0);
  ThreadState* thread = reinterpret_cast<ThreadState*>(v);
  SharedState* shared = thread->shared;

  while (true) {
    {
      MutexLock l(shared->GetMutex());
      if (shared->ShoudStopBgThread()) {
        shared->SetBgThreadFinish();
        shared->GetCondVar()->SignalAll();
        return;
      }
    }

    auto thread_pool_size_base = FLAGS_max_background_compactions;
    auto thread_pool_size_var = FLAGS_compaction_thread_pool_variations;
    int new_thread_pool_size =
        thread_pool_size_base - thread_pool_size_var +
        thread->rand.Next() % (thread_pool_size_var * 2 + 1);
    if (new_thread_pool_size < 1) {
      new_thread_pool_size = 1;
    }
    FLAGS_env->SetBackgroundThreads(new_thread_pool_size,
                                    rocksdb::Env::Priority::LOW);
    // Sleep up to 3 seconds
    FLAGS_env->SleepForMicroseconds(
        thread->rand.Next() % FLAGS_compaction_thread_pool_adjust_interval *
            1000 +
        1);
  }
}

void PrintKeyValue(int cf, uint64_t key, const char* value, size_t sz) {
  if (!FLAGS_verbose) {
    return;
  }
  std::string tmp;
  tmp.reserve(sz * 2 + 16);
  char buf[4];
  for (size_t i = 0; i < sz; i++) {
    snprintf(buf, 4, "%X", value[i]);
    tmp.append(buf);
  }
  fprintf(stdout, "[CF %d] %" PRIi64 " == > (%" ROCKSDB_PRIszt ") %s\n", cf,
          key, sz, tmp.c_str());
}

// Note that if hot_key_alpha != 0, it generates the key based on Zipfian
// distribution. Keys are randomly scattered to [0, FLAGS_max_key]. It does
// not ensure the order of the keys being generated and the keys does not have
// the active range which is related to FLAGS_active_width.
int64_t GenerateOneKey(ThreadState* thread, uint64_t iteration) {
  const double completed_ratio =
      static_cast<double>(iteration) / FLAGS_ops_per_thread;
  const int64_t base_key = static_cast<int64_t>(
      completed_ratio * (FLAGS_max_key - FLAGS_active_width));
  int64_t rand_seed = base_key + thread->rand.Next() % FLAGS_active_width;
  int64_t cur_key = rand_seed;
  if (FLAGS_hot_key_alpha != 0) {
    // If set the Zipfian distribution Alpha to non 0, use Zipfian
    double float_rand =
        (static_cast<double>(thread->rand.Next() % FLAGS_max_key)) /
        FLAGS_max_key;
    cur_key = GetOneHotKeyID(float_rand, FLAGS_max_key);
  }
  return cur_key;
}

// Note that if hot_key_alpha != 0, it generates the key based on Zipfian
// distribution. Keys being generated are in random order.
// If user want to generate keys based on uniform distribution, user needs to
// set hot_key_alpha == 0. It will generate the random keys in increasing
// order in the key array (ensure key[i] >= key[i+1]) and constrained in a
// range related to FLAGS_active_width.
std::vector<int64_t> GenerateNKeys(ThreadState* thread, int num_keys,
                                   uint64_t iteration) {
  const double completed_ratio =
      static_cast<double>(iteration) / FLAGS_ops_per_thread;
  const int64_t base_key = static_cast<int64_t>(
      completed_ratio * (FLAGS_max_key - FLAGS_active_width));
  std::vector<int64_t> keys;
  keys.reserve(num_keys);
  int64_t next_key = base_key + thread->rand.Next() % FLAGS_active_width;
  keys.push_back(next_key);
  for (int i = 1; i < num_keys; ++i) {
    // Generate the key follows zipfian distribution
    if (FLAGS_hot_key_alpha != 0) {
      double float_rand =
          (static_cast<double>(thread->rand.Next() % FLAGS_max_key)) /
          FLAGS_max_key;
      next_key = GetOneHotKeyID(float_rand, FLAGS_max_key);
    } else {
      // This may result in some duplicate keys
      next_key = next_key + thread->rand.Next() %
                                (FLAGS_active_width - (next_key - base_key));
    }
    keys.push_back(next_key);
  }
  return keys;
}

size_t GenerateValue(uint32_t rand, char* v, size_t max_sz) {
  size_t value_sz =
      ((rand % kRandomValueMaxFactor) + 1) * FLAGS_value_size_mult;
  assert(value_sz <= max_sz && value_sz >= sizeof(uint32_t));
  (void)max_sz;
  *((uint32_t*)v) = rand;
  for (size_t i = sizeof(uint32_t); i < value_sz; i++) {
    v[i] = (char)(rand ^ i);
  }
  v[value_sz] = '\0';
  return value_sz;  // the size of the value set.
}
}  // namespace rocksdb
#endif  // GFLAGS
