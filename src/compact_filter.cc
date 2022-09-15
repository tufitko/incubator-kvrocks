/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */

#include "compact_filter.h"
#include <string>
#include <utility>
#include <glog/logging.h>
#include "redis_bitmap.h"

namespace Engine {
using rocksdb::Slice;

bool MetadataFilter::Filter(int level,
                                    const Slice &key,
                                    const Slice &value,
                                    std::string *new_value,
                                    bool *modified) const {
  std::string ns, user_key, bytes = value.ToString();
  Metadata metadata(kRedisNone, false);
  rocksdb::Status s = metadata.Decode(bytes);
  ExtractNamespaceKey(key, &ns, &user_key, stor_->IsSlotIdEncoded());
  if (!s.ok()) {
    LOG(WARNING) << "[compact_filter/metadata] Failed to decode,"
                 << ", namespace: " << ns
                 << ", key: " << user_key
                 << ", err: " << s.ToString();
    return false;
  }
  DLOG(INFO) << "[compact_filter/metadata] "
             << "namespace: " << ns
             << ", key: " << user_key
             << ", result: " << (metadata.Expired() ? "deleted" : "reserved");
  return metadata.Expired();
}

bool SubKeyFilter::IsKeyExpired(const InternalKey &ikey, const Slice &value) const {
  std::string metadata_key;

  auto db = stor_->GetDB();
  const auto cf_handles = stor_->GetCFHandles();
  // storage close the would delete the column family handler and DB
  if (!db || cf_handles->size() < 2)  return false;

  ComposeNamespaceKey(ikey.GetNamespace(), ikey.GetKey(), &metadata_key, stor_->IsSlotIdEncoded());

  if (cached_key_.empty() || metadata_key != cached_key_) {
    std::string bytes;
    rocksdb::Status s = db->Get(rocksdb::ReadOptions(), (*cf_handles)[1], metadata_key, &bytes);
    cached_key_ = std::move(metadata_key);
    if (s.ok()) {
      cached_metadata_ = std::move(bytes);
    } else if (s.IsNotFound()) {
      // metadata was deleted(perhaps compaction or manual)
      // clear the metadata
      cached_metadata_.clear();
      return true;
    } else {
      LOG(ERROR) << "[compact_filter/subkey] Failed to fetch metadata"
                 << ", namespace: " << ikey.GetNamespace().ToString()
                 << ", key: " << ikey.GetKey().ToString()
                 << ", err: " << s.ToString();
      cached_key_.clear();
      cached_metadata_.clear();
      return false;
    }
  }
  // the metadata was not found
  if (cached_metadata_.empty()) return true;
  // the metadata is cached
  Metadata metadata(kRedisNone, false);
  rocksdb::Status s = metadata.Decode(cached_metadata_);
  if (!s.ok()) {
    cached_key_.clear();
    LOG(ERROR) << "[compact_filter/subkey] Failed to decode metadata"
               << ", namespace: " << ikey.GetNamespace().ToString()
               << ", key: " << ikey.GetKey().ToString()
               << ", err: " << s.ToString();
    return false;
  }
  if (metadata.Type() == kRedisString  // metadata key was overwrite by set command
      || metadata.Expired()
      || ikey.GetVersion() != metadata.version) {
    return true;
  }
  return metadata.Type() == kRedisBitmap && Redis::Bitmap::IsEmptySegment(value);
}

bool SubKeyFilter::Filter(int level,
                                  const Slice &key,
                                  const Slice &value,
                                  std::string *new_value,
                                  bool *modified) const {
  InternalKey ikey(key, stor_->IsSlotIdEncoded());
  bool result = IsKeyExpired(ikey, value);
  DLOG(INFO) << "[compact_filter/subkey] "
             << "namespace: " << ikey.GetNamespace().ToString()
             << ", metadata key: " << ikey.GetKey().ToString()
             << ", subkey: " << ikey.GetSubKey().ToString()
             << ", verison: " << ikey.GetVersion()
             << ", result: " << (result ? "deleted" : "reserved");
  return result;
}

rocksdb::CompactionFilter::Decision SubKeyFilter::IsKeyExpired(const InternalKey &ikey) const {
  std::string metadata_key;

  auto db = stor_->GetDB();
  const auto cf_handles = stor_->GetCFHandles();
  // storage close the would delete the column family handler and DB
  if (!db || cf_handles->size() < 2)  return rocksdb::CompactionFilter::Decision::kKeep;

  ComposeNamespaceKey(ikey.GetNamespace(), ikey.GetKey(), &metadata_key, stor_->IsSlotIdEncoded());

  if (cached_key_.empty() || metadata_key != cached_key_) {
    std::string bytes;
    rocksdb::Status s = db->Get(rocksdb::ReadOptions(), (*cf_handles)[1], metadata_key, &bytes);
    cached_key_ = std::move(metadata_key);
    if (s.ok()) {
      cached_metadata_ = std::move(bytes);
    } else if (s.IsNotFound()) {
      // metadata was deleted(perhaps compaction or manual)
      // clear the metadata
      cached_metadata_.clear();
      return rocksdb::CompactionFilter::Decision::kRemove;
    } else {
      LOG(ERROR) << "[compact_filter/subkey] Failed to fetch metadata"
                 << ", namespace: " << ikey.GetNamespace().ToString()
                 << ", key: " << ikey.GetKey().ToString()
                 << ", err: " << s.ToString();
      cached_key_.clear();
      cached_metadata_.clear();
      return rocksdb::CompactionFilter::Decision::kKeep;
    }
  }
  // the metadata was not found
  if (cached_metadata_.empty()) return rocksdb::CompactionFilter::Decision::kRemove;
  // the metadata is cached
  Metadata metadata(kRedisNone, false);
  rocksdb::Status s = metadata.Decode(cached_metadata_);
  if (!s.ok()) {
    cached_key_.clear();
    LOG(ERROR) << "[compact_filter/subkey] Failed to decode metadata"
               << ", namespace: " << ikey.GetNamespace().ToString()
               << ", key: " << ikey.GetKey().ToString()
               << ", err: " << s.ToString();
    return rocksdb::CompactionFilter::Decision::kKeep;
  }
  if (metadata.Type() == kRedisString  // metadata key was overwrite by set command
      || metadata.Expired()
      || ikey.GetVersion() != metadata.version) {
    return rocksdb::CompactionFilter::Decision::kRemove;
  }
  // todo: rework redisBitmap?
  return metadata.Type() == kRedisBitmap ? rocksdb::CompactionFilter::Decision::kUndetermined : rocksdb::CompactionFilter::Decision::kKeep;
}


rocksdb::CompactionFilter::Decision SubKeyFilter::FilterBlobByKey(int level, const Slice& key, std::string* new_value, std::string* skip_until) const {
  InternalKey ikey(key, stor_->IsSlotIdEncoded());
  return IsKeyExpired(ikey);
}


}  // namespace Engine
