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

#include "object/ob_object.h"
#define USING_LOG_PREFIX STORAGE_FTS

#include "share/rc/ob_server_runtime.h"
#include "storage/fts/ob_fts_stop_word.h"
#include "storage/fts/ob_fts_parser_helper.h"
#include "storage/fts/ob_fts_parser_property.h"

namespace oceanbase
{
namespace storage
{

namespace
{
// The built-in stopword list is immutable after initialization. Alphabetic
// ASCII has the same case folding in its UTF8MB4_GENERAL_CI collation, so it
// can use a compact read-only table instead of Unicode hashing and a bucket
// latch. Other characters still use the collation-aware set below.
struct AsciiStopWordTable
{
  static const int64_t KEY_MAX_LENGTH = 7;
  static const int64_t SLOT_COUNT = 128;

  AsciiStopWordTable() : keys_{}, valid_(true)
  {
    for (int64_t i = 0; valid_ && i < ARRAYSIZEOF(ob_stop_word_list); ++i) {
      const ObString word(STRLEN(ob_stop_word_list[i]), ob_stop_word_list[i]);
      uint64_t key = 0;
      if (word.length() > KEY_MAX_LENGTH || !make_key(word, key)) {
        // Keep the generic path if the built-in list gains other characters
        // or words too long for the packed key.
        valid_ = false;
      } else {
        int64_t slot = bucket(key);
        while (keys_[slot] != 0 && keys_[slot] != key) {
          slot = (slot + 1) % SLOT_COUNT;
        }
        keys_[slot] = key;
      }
    }
  }

  bool try_check(const ObString &word, bool &is_stopword) const
  {
    uint64_t key = 0;
    if (!valid_ || !make_key(word, key)) {
      return false;
    }
    is_stopword = false;
    if (word.length() <= KEY_MAX_LENGTH) {
      int64_t slot = bucket(key);
      while (keys_[slot] != 0 && keys_[slot] != key) {
        slot = (slot + 1) % SLOT_COUNT;
      }
      is_stopword = keys_[slot] == key;
    }
    return true;
  }

private:
  static bool make_key(const ObString &word, uint64_t &key)
  {
    key = 0;
    for (int64_t i = 0; i < word.length(); ++i) {
      unsigned char ch = static_cast<unsigned char>(word.ptr()[i]);
      if (ch >= 'A' && ch <= 'Z') {
        ch += 'a' - 'A';
      }
      if (ch < 'a' || ch > 'z') {
        return false;
      }
      if (i < KEY_MAX_LENGTH) {
        key = (key << 8) | ch;
      }
    }
    return !word.empty();
  }

  static int64_t bucket(const uint64_t key)
  {
    return ((key ^ (key >> 32)) * UINT64_C(11400714819323198485)) >> 57;
  }

  uint64_t keys_[SLOT_COUNT];
  bool valid_;
};

const AsciiStopWordTable ASCII_STOP_WORDS;
} // namespace

////////////////////////////////////////////////////////////////////////////////
// class ObStopWordChecker
ObStopWordChecker::~ObStopWordChecker()
{
  destroy();
}

int ObStopWordChecker::init()
{
  int ret = OB_SUCCESS;
  
  

  if (inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(stopword_set_.create(DEFAULT_STOPWORD_BUCKET_NUM, "StopWordSet", "StopWordSet"))) {
  } else {
    ObObjMeta stop_meta;
    stop_meta.set_varchar();
    stop_meta.set_collation_type(ObCollationType::CS_TYPE_UTF8MB4_GENERAL_CI);

    stopword_type_.set_meta(stop_meta);
    const int64_t stopword_count = sizeof(ob_stop_word_list) / sizeof(ob_stop_word_list[0]);
    for (int64_t i = 0; OB_SUCC(ret) && i < stopword_count; ++i) {
      ObFTWord stopword(STRLEN(ob_stop_word_list[i]), ob_stop_word_list[i], stopword_type_);
      if (OB_FAIL(stopword_set_.set_refactored(stopword))) {
      }
    }

    if (OB_SUCC(ret)) {
      inited_ = true;
    }
  }
  return ret;
}

void ObStopWordChecker::destroy()
{
  if (inited_) {
    stopword_set_.destroy();
    inited_ = false;
  }
}

int ObStopWordChecker::check_stopword(const ObFTWord &word, bool &is_stopword)
{
  int ret = OB_SUCCESS;
  
  
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(word.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (common::ObCharset::is_valid_collation(word.get_collation_type())
             && common::ObCharset::charset_type_by_coll(word.get_collation_type())
                == common::ObCharset::charset_type_by_coll(stopword_type_.get_collation_type())) {
    // Collation conversion within the same charset does not change bytes.
    // Keep the stopword collation for hashing/comparison, but avoid creating
    // a temporary arena for every token on the usual UTF-8 path.
    const common::ObString key = word.get_word().get_string();
    if (!ASCII_STOP_WORDS.try_check(key, is_stopword)) {
      ObFTWord converted(key.length(), key.ptr(), stopword_type_);
      ret = stopword_set_.exist_refactored(converted);
      if (OB_HASH_NOT_EXIST == ret || OB_HASH_EXIST == ret) {
        is_stopword = OB_HASH_EXIST == ret;
        ret = OB_SUCCESS;
      } else if (OB_SUCC(ret)) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
  } else {
    common::ObArenaAllocator allocator(lib::ObMemAttr("ChkStopWord"));
    common::ObString cmp_str;
    // do nothing set out with in if type is the same.
    if (OB_FAIL(common::ObCharset::charset_convert(
                                       allocator,
                                       word.get_word().get_string(),
                                       word.get_collation_type(),
                                       stopword_type_.get_collation_type(),
                                       cmp_str))) {
    } else {
      ObFTWord converted(cmp_str.length(), cmp_str.ptr(), stopword_type_);
      ret = stopword_set_.exist_refactored(converted);
      if (OB_HASH_NOT_EXIST == ret) {
        is_stopword = false;
        ret = OB_SUCCESS;
      } else if (OB_HASH_EXIST == ret) {
        is_stopword = true;
        ret = OB_SUCCESS;
      } else if (OB_SUCC(ret)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
      }
    }
  }
  return ret;
}

////////////////////////////////////////////////////////////////////////////////
// class ObAddWord
ObAddWord::ObAddWord(
    const ObFTParserProperty &property,
    const ObObjMeta &meta,
    const ObAddWordFlag &flag,
    common::ObIAllocator &allocator,
    ObFTWordMap &word_map)
  : word_meta_(meta),
    allocator_(allocator),
    word_map_(word_map),
    min_max_word_cnt_(0),
    non_stopword_cnt_(0),
    stopword_cnt_(0),
    min_token_size_(property.min_token_size_),
    max_token_size_(property.max_token_size_),
    flag_(flag)
{
}

int ObAddWord::process_word(
    const char *word,
    const int64_t word_len,
    const int64_t char_cnt,
    const int64_t word_freq)
{
  int ret = OB_SUCCESS;
  bool is_stopword = false;
  ObFTWord src_word(word_len, word, word_meta_);
  ObFTWord dst_word;
  if (OB_ISNULL(word) || OB_UNLIKELY(0 >= word_len || 0 >= char_cnt || 0 >= word_freq)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (is_min_max_word(char_cnt)) {
    ++min_max_word_cnt_;
  } else if (OB_FAIL(casedown_word(src_word, dst_word))) {
  } else if (OB_FAIL(check_stopword(dst_word, is_stopword))) {
  } else if (OB_UNLIKELY(is_stopword)) {
    ++stopword_cnt_;
  } else if (OB_FAIL(groupby_word(dst_word, word_freq))) {
  } else {
    non_stopword_cnt_ += word_freq;
  }
  return ret;
}

bool ObAddWord::is_min_max_word(const int64_t c_len) const
{
  return flag_.min_max_word() && (c_len < min_token_size_ || c_len > max_token_size_);
}

int ObAddWord::casedown_word(const ObFTWord &src, ObFTWord &dst)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(src.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (flag_.casedown()) {
    ObString dst_str;
    const ObString src_str = src.get_word().get_string();
    const ObCollationType coll = word_meta_.get_collation_type();
    // U+4E00..U+9FFF has no case mappings. Recognize well-formed UTF-8
    // basic CJK words without decoding them again in the case converter.
    // Still copy into allocator_: map keys must outlive the token iterator.
    bool basic_cjk = (coll == CS_TYPE_UTF8MB4_GENERAL_CI || coll == CS_TYPE_UTF8MB4_BIN)
        && src_str.length() % 3 == 0;
    const unsigned char *bytes = reinterpret_cast<const unsigned char *>(src_str.ptr());
    for (int64_t i = 0; basic_cjk && i < src_str.length(); i += 3) {
      basic_cjk = bytes[i] >= 0xe4 && bytes[i] <= 0xe9
          && (bytes[i + 1] & 0xc0) == 0x80 && (bytes[i + 2] & 0xc0) == 0x80
          && (bytes[i] != 0xe4 || bytes[i + 1] >= 0xb8);
    }
    if (basic_cjk) {
      ret = ob_write_string(allocator_, src_str, dst_str);
    } else {
      ret = ObCharset::tolower(coll, src_str, dst_str, allocator_);
    }
    if (OB_SUCC(ret)) {
      ObFTWord tmp(dst_str.length(), dst_str.ptr(), word_meta_);
      dst = tmp;
    }
  } else {
    dst = src;
  }
  return ret;
}

int ObAddWord::check_stopword(const ObFTWord &ft_word, bool &is_stopword)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(ft_word.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (flag_.stopword()) {
    ObStopWordChecker *stop_word_checker = ObFTParseData::instance().stop_word_checker();
    if (OB_ISNULL(stop_word_checker)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(stop_word_checker->check_stopword(ft_word, is_stopword))) {
    }
  }
  return ret;
}

int ObAddWord::groupby_word(const ObFTWord &word, const int64_t word_freq)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(word.empty() || word_freq <= 0)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (!flag_.groupby_word()) {
    if (OB_FAIL(word_map_.set_refactored(word, 1/*word count*/))) {
    }
  } else {
    // Insert or accumulate with one hash lookup. Preserve the first occurrence
    // count and the existing key storage, as the previous get/set pair did.
    auto accumulate = [word_freq](common::hash::HashMapPair<ObFTWord, int64_t> &entry) {
      entry.second += word_freq;
    };
    if (OB_FAIL(word_map_.set_or_update(word, 1/*word count*/, accumulate))) {
    }
  }
  return ret;
}

} // end namespace storage
} // end namespace oceanbase
