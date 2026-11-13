/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OB_FTS_STRUCT_H_
#define OB_FTS_STRUCT_H_

#include "lib/charset/ob_charset.h"
#include "lib/hash/ob_hashmap.h"
#include "object/ob_object.h"
#include "share/datum/ob_datum_funcs.h"

namespace oceanbase
{
namespace storage
{

class ObFTWord final
{
public:
  ObFTWord() : word_(), meta_() {}
  ObFTWord(const int64_t length, const char *ptr, const ObObjMeta &meta) : meta_(meta)
  {
    word_.set_string(ptr, length);
  }
  ~ObFTWord() = default;

  OB_INLINE const ObDatum &get_word() const { return word_; }
  OB_INLINE ObCollationType get_collation_type() const { return meta_.get_collation_type(); }
  OB_INLINE bool empty() const { return word_.get_string().empty(); }
  int hash(uint64_t &hash_val) const;
  bool operator==(const ObFTWord &other) const;
  OB_INLINE bool operator !=(const ObFTWord &other) const { return !(other == *this); }

  TO_STRING_KV(K_(meta), K_(word));

private:
  ObDatum word_;
  ObObjMeta meta_;
};

typedef common::hash::ObHashMap<ObFTWord, int64_t> ObFTWordMap;

// 词频哈希表桶数估算
//
// 词频哈希表按“不同词”聚类，装载因子（不同词数 / 桶数）过高时，一次查找要沿桶链
// 逐个比较 key，而 key 的比较走的是带排序规则的字符串比较（逐字符取权重），
// 是分词与索引构建路径上最贵的操作之一。
//
// 不同词数与文本长度大致成正比：英文约 5~6 字节/词（含分隔符），中文约 6 字节/词，
// 这里按 4 字节/词估算，使装载因子保持在 1 以内；同时设置桶数上限，避免超长文本
// 占用过多内存。上限 8191 桶在长文档下仍能把装载因子从原来的 20 降到 2.4 以内。
static constexpr int64_t OB_FT_WORD_BUCKET_NUM_PER_BYTE = 4;
static constexpr int64_t OB_FT_WORD_MAX_BUCKET_NUM = 8191;
// 查询文本通常很短，桶数上限取小一些，避免为一次查询申请过多内存
static constexpr int64_t OB_FT_QUERY_WORD_MAX_BUCKET_NUM = 1024;
static constexpr int64_t OB_FT_WORD_MIN_BUCKET_NUM = 2;

inline int64_t ob_ft_word_bucket_num(const int64_t text_len, const int64_t max_bucket_num)
{
  const int64_t estimate = (text_len > 0) ? (text_len / OB_FT_WORD_BUCKET_NUM_PER_BYTE)
                                          : OB_FT_WORD_MIN_BUCKET_NUM;
  const int64_t bucket_num = estimate < OB_FT_WORD_MIN_BUCKET_NUM ? OB_FT_WORD_MIN_BUCKET_NUM
                                                                  : estimate;
  return bucket_num < max_bucket_num ? bucket_num : max_bucket_num;
}

class ObAddWordFlag final
{
private:
  static const uint64_t AWF_NONE         = 0;
  static const uint64_t AWF_MIN_MAX_WORD = 1 << 0; // filter words that are less than a minimum or greater
                                                   // than a maximum word length.
  static const uint64_t AWF_STOPWORD     = 1 << 1; // filter by sotp word table.
  static const uint64_t AWF_CASEDOWN     = 1 << 2; // convert characters from uppercase to lowercase.
  static const uint64_t AWF_GROUPBY_WORD = 1 << 3; // distinct and word aggregation
public:
  ObAddWordFlag() : flag_(AWF_NONE) {}
  ~ObAddWordFlag() = default;
public:
  void set_flag(const uint64_t flag) { flag_ |= flag; }
private:
  void clear_flag(const uint64_t flag) { flag_ &= ~flag; }
  bool has_flag(const uint64 flag) const { return (flag_ & flag) == flag; }
public:
  void set_min_max_word() { set_flag(AWF_MIN_MAX_WORD); }
  void set_stop_word() { set_flag(AWF_STOPWORD); }
  void set_casedown() { set_flag(AWF_CASEDOWN); }
  void set_groupby_word() { set_flag(AWF_GROUPBY_WORD); }
  void clear() { flag_ = AWF_NONE; }
  void clear_min_max_word() { clear_flag(AWF_MIN_MAX_WORD); }
  void clear_stop_word() { clear_flag(AWF_STOPWORD); }
  void clear_casedown() { clear_flag(AWF_CASEDOWN); }
  void clear_groupby_word() { clear_flag(AWF_GROUPBY_WORD); }
  bool min_max_word() const { return has_flag(AWF_MIN_MAX_WORD); }
  bool stopword() const { return has_flag(AWF_STOPWORD); }
  bool casedown() const { return has_flag(AWF_CASEDOWN); }
  bool groupby_word() const { return has_flag(AWF_GROUPBY_WORD); }
  TO_STRING_KV(K_(flag));
private:
  uint64_t flag_;
};

} // end namespace storage
} // end namespace oceanbase

#endif// OB_FTS_STRUCT_H_
