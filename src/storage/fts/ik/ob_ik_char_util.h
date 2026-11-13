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

#ifndef _OCEANBASE_STORAGE_FTS_IK_OB_IK_CHAR_UTIL_H_
#define _OCEANBASE_STORAGE_FTS_IK_OB_IK_CHAR_UTIL_H_

#include "lib/charset/ob_charset.h"
#include "lib/charset/ob_charset_string_helper.h"
#include "lib/charset/ob_ctype.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/utils/unicode_utils.h"

#include <cstdint>
#include <type_traits>

namespace oceanbase
{
namespace storage
{
// See: lang/Character.java
class ObFTCharUtil
{
public:
  enum class CharType : int8_t
  {
    USELESS = 0,
    ARABIC_LETTER = 1,
    ENGLISH_LETTER = 2,
    CHINESE = 3,
    OTHER_CJK = 4,
    SURROGATE_HIGH = 5,
    SURROGATE_LOW = 6,
  };

  static int classify_first_char(ObCollationType coll_type,
                                 const char *input,
                                 const uint8_t char_len,
                                 CharType &type);

  static int check_cn_number(ObCollationType coll_type,
                             const char *input,
                             const uint8_t char_len,
                             bool &is_cn_number);

  static int check_num_connector(ObCollationType coll_type,
                                 const char *input,
                                 const uint8_t char_len,
                                 bool &is_connector);

  static int check_letter_connector(ObCollationType coll_type,
                                    const char *input,
                                    const uint8_t char_len,
                                    bool &is_connector);

  // some cjk word should be ignored
  static int is_ignore_single_cjk(ObCollationType coll_type,
                                  const char *input,
                                  const uint8_t char_len,
                                  bool &ignore);

  /**
   * Decode the first char of the input into a unicode code point.
   * The decoding is exactly the one used by classify_first_char, so that a
   * caller which already knows the code point of the current char does not
   * have to decode it again.
   */
  static int decode_first_char(ObCollationType coll_type,
                                const char *input,
                                const uint8_t char_len,
                                ob_wc_t &unicode);

  /**
   * Classify an already decoded unicode code point.
   * Semantics are identical to classify_first_char, but no decoding is done.
   */
  static int classify_unicode(const ob_wc_t unicode, CharType &type);

  // Code point variants of the checkers above, for callers which already
  // decoded the current char. The segmentation hot path decodes every char
  // once and then reuses the code point for all the checks.
  static int check_cn_number_unicode(ObCollationType coll_type,
                                      const ob_wc_t unicode,
                                      bool &is_cn_number);

  static int check_num_connector_unicode(ObCollationType coll_type,
                                         const ob_wc_t unicode,
                                         bool &is_connector);

  static int check_letter_connector_unicode(ObCollationType coll_type,
                                            const ob_wc_t unicode,
                                            bool &is_connector);

  static int is_ignore_single_cjk_unicode(ObCollationType coll_type,
                                          const ob_wc_t unicode,
                                          bool &ignore);

private:
  template <ObCharsetType CS_TYPE>
  static int do_classify(const char *input, const uint8_t char_len, CharType &type);

private:
  /*************************  Ignore: some char should not output itself *********************/
  template <ObCharsetType CS_TYPE>
  static typename std::enable_if<(CS_TYPE == CHARSET_UTF8MB4 || CS_TYPE == CHARSET_UTF16
                                  || CS_TYPE == CHARSET_UTF16LE),
                                 int>::type
  is_ignore(const char *input, const uint8_t char_len, bool &ignore);
  /****************************************************************************************/

  /*************************  CN Number: one two three two *********************/
  template <ObCharsetType CS_TYPE>
  static typename std::enable_if<(CS_TYPE == CHARSET_UTF8MB4 || CS_TYPE == CHARSET_UTF16
                                  || CS_TYPE == CHARSET_UTF16LE),
                                 int>::type
  is_cn_number(const char *input, const uint8_t char_len, bool &is_cn_number);
  /****************************************************************************************/

  /*************************  English letter connector: #,-@ *********************/
  template <ObCharsetType CS_TYPE>
  static typename std::enable_if<(CS_TYPE == CHARSET_UTF8MB4 || CS_TYPE == CHARSET_UTF16
                                  || CS_TYPE == CHARSET_UTF16LE),
                                 int>::type
  is_letter_connector(const char *input, const uint8_t char_len, bool &is_connector);
  /****************************************************************************************/

  /*************************  Number letter connector: ,. *********************/
  template <ObCharsetType CS_TYPE>
  static typename std::enable_if<(CS_TYPE == CHARSET_UTF8MB4 || CS_TYPE == CHARSET_UTF16
                                  || CS_TYPE == CHARSET_UTF16LE),
                                 int>::type
  is_num_connector(const char *input, const uint8_t char_len, bool &is_connector);
  /****************************************************************************************/

private:
  // only for unicode charset
  template <ObCharsetType CS_TYPE>
  static typename std::enable_if<(CS_TYPE == CHARSET_UTF8MB4 || CS_TYPE == CHARSET_UTF16
                                  || CS_TYPE == CHARSET_UTF16LE),
                                 int>::type
  decode_unicode(const char *input, const uint8_t char_len, ob_wc_t &unicode)
  {
    int ret = OB_SUCCESS;
    if (OB_ISNULL(input)) {
      ret = OB_INVALID_ARGUMENT;
      STORAGE_FTS_LOG(WARN, "Invalid input", K(input), K(ret));
    } else {
      const unsigned char *ustart = reinterpret_cast<const unsigned char *>(input);
      const unsigned char *uend = reinterpret_cast<const unsigned char *>(&input[char_len]);
      int code_size = common::ob_charset_decode_unicode<CS_TYPE>(ustart, uend, unicode);
      if (code_size < 0) {
        ret = OB_UNEXPECT_INTERNAL_ERROR;
        STORAGE_FTS_LOG(WARN, "Failed to decode unicode", K(code_size), K(ret));
      }
    }
    return ret;
  }
}; // End of class.

template <ObCharsetType CS_TYPE>
inline typename std::enable_if<(CS_TYPE == CHARSET_UTF8MB4 || CS_TYPE == CHARSET_UTF16
                                || CS_TYPE == CHARSET_UTF16LE),
                               int>::type
ObFTCharUtil::is_ignore(const char *input, const uint8_t char_len, bool &ignore)
{
  int ret = OB_SUCCESS;
  ob_wc_t unicode = 0;
  if (OB_FAIL(decode_unicode<CS_TYPE>(input, char_len, unicode))) {
  } else {
    ignore = ObUnicodeBlockUtils::check_ignore_as_single(unicode);
  }
  return ret;
}

template <ObCharsetType CS_TYPE>
inline typename std::enable_if<(CS_TYPE == CHARSET_UTF8MB4 || CS_TYPE == CHARSET_UTF16
                                || CS_TYPE == CHARSET_UTF16LE),
                               int>::type
ObFTCharUtil::is_cn_number(const char *input, const uint8_t char_len, bool &is_cn_number)
{
  int ret = OB_SUCCESS;
  ob_wc_t unicode = 0;
  if (OB_FAIL(decode_unicode<CS_TYPE>(input, char_len, unicode))) {
  } else {
    is_cn_number = ObUnicodeBlockUtils::is_unicode_cn_number(unicode);
  }
  return ret;
}

template <ObCharsetType CS_TYPE>
inline typename std::enable_if<(CS_TYPE == CHARSET_UTF8MB4 || CS_TYPE == CHARSET_UTF16
                                || CS_TYPE == CHARSET_UTF16LE),
                               int>::type
ObFTCharUtil::is_letter_connector(const char *input, const uint8_t char_len, bool &is_connector)
{
  int ret = OB_SUCCESS;
  ob_wc_t unicode = 0;
  if (OB_FAIL(decode_unicode<CS_TYPE>(input, char_len, unicode))) {
  } else {
    is_connector = ObUnicodeBlockUtils::check_letter_connector(unicode);
  }
  return ret;
}

template <ObCharsetType CS_TYPE>
inline typename std::enable_if<(CS_TYPE == CHARSET_UTF8MB4 || CS_TYPE == CHARSET_UTF16
                                || CS_TYPE == CHARSET_UTF16LE),
                               int>::type
ObFTCharUtil::is_num_connector(const char *input, const uint8_t char_len, bool &is_connector)
{
  int ret = OB_SUCCESS;
  ob_wc_t unicode = 0;
  if (OB_FAIL(decode_unicode<CS_TYPE>(input, char_len, unicode))) {
  } else {
    is_connector = ObUnicodeBlockUtils::check_num_connector(unicode);
  }
  return ret;
}

// Support GBK

// Implementation of some frame;

inline int ObFTCharUtil::check_cn_number(ObCollationType coll_type,
                                         const char *input,
                                         const uint8_t char_len,
                                         bool &is_cn_number)
{
  int ret = OB_SUCCESS;
  ObCharsetType cs_type = ObCharset::charset_type_by_coll(coll_type);

  switch (cs_type) {
  case CHARSET_UTF8MB4: {
    ret = ObFTCharUtil::is_cn_number<CHARSET_UTF8MB4>(input, char_len, is_cn_number);
    break;
  }
  default:
    ret = OB_NOT_SUPPORTED;
    STORAGE_FTS_LOG(WARN, "Not supported charset type", K(ret), K(cs_type));
  }
  return ret;
}

inline int ObFTCharUtil::check_num_connector(ObCollationType coll_type,
                                             const char *input,
                                             const uint8_t char_len,
                                             bool &is_connector)
{
  int ret = OB_SUCCESS;
  ObCharsetType cs_type = ObCharset::charset_type_by_coll(coll_type);

  switch (cs_type) {
  case CHARSET_UTF8MB4: {
    ret = ObFTCharUtil::is_num_connector<CHARSET_UTF8MB4>(input, char_len, is_connector);
    break;
  }
  default:
    ret = OB_NOT_SUPPORTED;
    STORAGE_FTS_LOG(WARN, "Not supported charset type", K(ret), K(cs_type));
  }
  return ret;
}

inline int ObFTCharUtil::check_letter_connector(ObCollationType coll_type,
                                                const char *input,
                                                const uint8_t char_len,
                                                bool &is_connector)
{
  int ret = OB_SUCCESS;
  ObCharsetType cs_type = ObCharset::charset_type_by_coll(coll_type);

  switch (cs_type) {
  case CHARSET_UTF8MB4: {
    // ret = ObFTCharUtil::do_check_letter_connector<CHARSET_UTF8MB4>(input, char_len,
    // is_connector);
    ret = ObFTCharUtil::is_letter_connector<CHARSET_UTF8MB4>(input, char_len, is_connector);
    break;
  }
  default:
    ret = OB_NOT_SUPPORTED;
    STORAGE_FTS_LOG(WARN, "Not supported charset type", K(ret), K(cs_type));
  }
  return ret;
}

inline int ObFTCharUtil::is_ignore_single_cjk(ObCollationType coll_type,
                                              const char *input,
                                              const uint8_t char_len,
                                              bool &ignore)
{
  int ret = OB_SUCCESS;
  ObCharsetType cs_type = ObCharset::charset_type_by_coll(coll_type);

  switch (cs_type) {
  case CHARSET_UTF8MB4: {
    ret = ObFTCharUtil::is_ignore<CHARSET_UTF8MB4>(input, char_len, ignore);
    break;
  }
  default:
    ret = OB_NOT_SUPPORTED;
    STORAGE_FTS_LOG(WARN, "Not supported charset type", K(ret), K(cs_type));
    break;
  }
  return ret;
}

template <ObCharsetType CS_TYPE>
inline int ObFTCharUtil::do_classify(const char *input, const uint8_t char_len, CharType &type)
{
  int ret = OB_SUCCESS;
  ob_wc_t unicode = 0;
  type = CharType::USELESS;

  // 字符分类是分词热路径上每字符都要走的判断。原实现依次调用 is_alpha /
  // is_arabic / is_chinese / is_other_cjk / is_surrogate_high /
  // is_surrogate_low，每个判断都会各自把同一个字符解码一次，等于把同一个
  // UTF-8 序列重复解码 4~6 次。这里只解码一次，之后所有 Unicode 区间判断
  // 都基于同一个码点进行。
  // 语义完全等价：
  //  - UTF8MB4：原实现的代理项判断直接返回 false（不解码），而合法 UTF-8
  //    解码出来的码点不可能落在 0xD800~0xDFFF 区间（解码器会拒绝），因此
  //    基于码点的判断结果与原来一致。
  //  - UTF16/UTF16LE：通用解码模板返回的码点恒为 0，原实现的各项判断同样
  //    基于 0 进行，结果不变。
  if (OB_FAIL(decode_unicode<CS_TYPE>(input, char_len, unicode))) {
  } else if (OB_FAIL(classify_unicode(unicode, type))) {
  }
  return ret;
}

inline int ObFTCharUtil::classify_first_char(ObCollationType coll_type,
                                             const char *input,
                                             const uint8_t char_len,
                                             CharType &type)
{
  int ret = OB_SUCCESS;
  ObCharsetType cs_type = ObCharset::charset_type_by_coll(coll_type);

  switch (cs_type) {
  case CHARSET_UTF8MB4: {
    ret = do_classify<CHARSET_UTF8MB4>(input, char_len, type);
    break;
  }
  case CHARSET_UTF16: {
    ret = do_classify<CHARSET_UTF16>(input, char_len, type);
    break;
  }
  case CHARSET_UTF16LE: {
    ret = do_classify<CHARSET_UTF16LE>(input, char_len, type);
    break;
  }
  default:
    ret = OB_NOT_SUPPORTED;
    STORAGE_FTS_LOG(WARN, "Not supported charset type", K(ret), K(cs_type));
    break;
  }
  return ret;
}

inline int ObFTCharUtil::decode_first_char(ObCollationType coll_type,
                                           const char *input,
                                           const uint8_t char_len,
                                           ob_wc_t &unicode)
{
  int ret = OB_SUCCESS;
  ObCharsetType cs_type = ObCharset::charset_type_by_coll(coll_type);

  switch (cs_type) {
  case CHARSET_UTF8MB4: {
    ret = ObFTCharUtil::decode_unicode<CHARSET_UTF8MB4>(input, char_len, unicode);
    break;
  }
  case CHARSET_UTF16: {
    ret = ObFTCharUtil::decode_unicode<CHARSET_UTF16>(input, char_len, unicode);
    break;
  }
  case CHARSET_UTF16LE: {
    ret = ObFTCharUtil::decode_unicode<CHARSET_UTF16LE>(input, char_len, unicode);
    break;
  }
  default:
    ret = OB_NOT_SUPPORTED;
    STORAGE_FTS_LOG(WARN, "Not supported charset type", K(ret), K(cs_type));
    break;
  }
  return ret;
}

inline int ObFTCharUtil::classify_unicode(const ob_wc_t unicode, CharType &type)
{
  int ret = OB_SUCCESS;
  type = CharType::USELESS;

  if (ObUnicodeBlockUtils::is_alpha(unicode)) {
    type = CharType::ENGLISH_LETTER;
  } else if (ObUnicodeBlockUtils::is_arabic(unicode)) {
    type = CharType::ARABIC_LETTER;
  } else if (ObUnicodeBlockUtils::is_chinese(unicode)) {
    type = CharType::CHINESE;
  } else if (ObUnicodeBlockUtils::is_other_cjk(unicode)) {
    type = CharType::OTHER_CJK;
  } else if (ObUnicodeBlockUtils::check_high_surrogate(unicode)) {
    type = CharType::SURROGATE_HIGH;
  } else if (ObUnicodeBlockUtils::check_low_surrogate(unicode)) {
    type = CharType::SURROGATE_LOW;
  }
  return ret;
}

inline int ObFTCharUtil::check_cn_number_unicode(ObCollationType coll_type,
                                                  const ob_wc_t unicode,
                                                  bool &is_cn_number)
{
  int ret = OB_SUCCESS;
  ObCharsetType cs_type = ObCharset::charset_type_by_coll(coll_type);

  switch (cs_type) {
  case CHARSET_UTF8MB4: {
    is_cn_number = ObUnicodeBlockUtils::is_unicode_cn_number(unicode);
    break;
  }
  default:
    ret = OB_NOT_SUPPORTED;
    STORAGE_FTS_LOG(WARN, "Not supported charset type", K(ret), K(cs_type));
  }
  return ret;
}

inline int ObFTCharUtil::check_num_connector_unicode(ObCollationType coll_type,
                                                      const ob_wc_t unicode,
                                                      bool &is_connector)
{
  int ret = OB_SUCCESS;
  ObCharsetType cs_type = ObCharset::charset_type_by_coll(coll_type);

  switch (cs_type) {
  case CHARSET_UTF8MB4: {
    is_connector = ObUnicodeBlockUtils::check_num_connector(unicode);
    break;
  }
  default:
    ret = OB_NOT_SUPPORTED;
    STORAGE_FTS_LOG(WARN, "Not supported charset type", K(ret), K(cs_type));
  }
  return ret;
}

inline int ObFTCharUtil::check_letter_connector_unicode(ObCollationType coll_type,
                                                         const ob_wc_t unicode,
                                                         bool &is_connector)
{
  int ret = OB_SUCCESS;
  ObCharsetType cs_type = ObCharset::charset_type_by_coll(coll_type);

  switch (cs_type) {
  case CHARSET_UTF8MB4: {
    is_connector = ObUnicodeBlockUtils::check_letter_connector(unicode);
    break;
  }
  default:
    ret = OB_NOT_SUPPORTED;
    STORAGE_FTS_LOG(WARN, "Not supported charset type", K(ret), K(cs_type));
  }
  return ret;
}

inline int ObFTCharUtil::is_ignore_single_cjk_unicode(ObCollationType coll_type,
                                                       const ob_wc_t unicode,
                                                       bool &ignore)
{
  int ret = OB_SUCCESS;
  ObCharsetType cs_type = ObCharset::charset_type_by_coll(coll_type);

  switch (cs_type) {
  case CHARSET_UTF8MB4: {
    ignore = ObUnicodeBlockUtils::check_ignore_as_single(unicode);
    break;
  }
  default:
    ret = OB_NOT_SUPPORTED;
    STORAGE_FTS_LOG(WARN, "Not supported charset type", K(ret), K(cs_type));
    break;
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase

#endif // _OCEANBASE_STORAGE_FTS_IK_OB_IK_CHAR_UTIL_H_
