/**
 * Copyright (c) 2021 OceanBase
 * SPDX-License-Identifier: Apache-2.0
 */

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/ob_file_table_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <algorithm>
#include <string>
#include <vector>
#include <filesystem>

#include "share/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/container/ob_se_array.h"
#include "lib/utility/ob_print_utils.h"

namespace oceanbase
{
namespace sql
{

using namespace common;

namespace
{

const int64_t FILE_READ_BUFFER_SIZE = 1 << 16;

enum InferredKind
{
  IK_UNKNOWN = 0,
  IK_BOOL,
  IK_INT,
  IK_DOUBLE,
  IK_DATE,
  IK_DATETIME,
  IK_STRING,
};

bool is_bool_text(const char *s, int64_t len)
{
  return (4 == len && 0 == strncasecmp(s, "true", 4)) ||
         (5 == len && 0 == strncasecmp(s, "false", 5));
}

bool is_int_text(const char *s, int64_t len)
{
  bool bret = true;
  int64_t i = 0;
  if (i < len && ('-' == s[i] || '+' == s[i])) {
    ++i;
  }
  if (i >= len) {
    bret = false;
  }
  for (; bret && i < len; ++i) {
    if (!isdigit(static_cast<unsigned char>(s[i]))) {
      bret = false;
    }
  }
  return bret;
}

bool is_double_text(const char *s, int64_t len)
{
  bool bret = true;
  bool has_digit = false;
  bool has_dot = false;
  int64_t i = 0;
  if (i < len && ('-' == s[i] || '+' == s[i])) {
    ++i;
  }
  for (; i < len && isdigit(static_cast<unsigned char>(s[i])); ++i) {
    has_digit = true;
  }
  if (i < len && '.' == s[i]) {
    has_dot = true;
    ++i;
    for (; i < len && isdigit(static_cast<unsigned char>(s[i])); ++i) {
      has_digit = true;
    }
  }
  if (!has_digit) {
    bret = false;
  }
  if (bret && i < len && ('e' == s[i] || 'E' == s[i])) {
    ++i;
    if (i < len && ('-' == s[i] || '+' == s[i])) {
      ++i;
    }
    bool has_exp_digit = false;
    for (; i < len && isdigit(static_cast<unsigned char>(s[i])); ++i) {
      has_exp_digit = true;
    }
    if (!has_exp_digit) {
      bret = false;
    }
  }
  return bret && i == len && has_dot;
}

bool is_date_text(const char *s, int64_t len)
{
  bool bret = (10 == len);
  for (int64_t i = 0; bret && i < len; ++i) {
    if (4 == i || 7 == i) {
      bret = ('-' == s[i]);
    } else {
      bret = isdigit(static_cast<unsigned char>(s[i]));
    }
  }
  return bret;
}

bool is_datetime_text(const char *s, int64_t len)
{
  bool bret = (len >= 19);
  for (int64_t i = 0; bret && i < len; ++i) {
    if (4 == i || 7 == i) {
      bret = ('-' == s[i]);
    } else if (10 == i) {
      bret = (' ' == s[i] || 'T' == s[i]);
    } else if (13 == i || 16 == i) {
      bret = (':' == s[i]);
    } else if (19 == i && '.' == s[i]) {
      // fraction part, len check below
    } else if (19 < i) {
      bret = isdigit(static_cast<unsigned char>(s[i]));
    } else {
      bret = isdigit(static_cast<unsigned char>(s[i]));
    }
  }
  if (bret && len > 19) {
    bret = ('.' == s[19]);
  }
  return bret;
}

InferredKind detect_kind(const char *s, int64_t len)
{
  InferredKind kind = IK_STRING;
  if (0 == len) {
    kind = IK_UNKNOWN;
  } else if (is_bool_text(s, len)) {
    kind = IK_BOOL;
  } else if (is_int_text(s, len)) {
    kind = IK_INT;
  } else if (is_double_text(s, len)) {
    kind = IK_DOUBLE;
  } else if (is_date_text(s, len)) {
    kind = IK_DATE;
  } else if (is_datetime_text(s, len)) {
    kind = IK_DATETIME;
  } else {
    kind = IK_STRING;
  }
  return kind;
}

InferredKind merge_kind(InferredKind a, InferredKind b)
{
  InferredKind ret = IK_STRING;
  if (IK_UNKNOWN == a) {
    ret = b;
  } else if (IK_UNKNOWN == b) {
    ret = a;
  } else if (a == b) {
    ret = a;
  } else if ((IK_INT == a && IK_DOUBLE == b) || (IK_DOUBLE == a && IK_INT == b)) {
    ret = IK_DOUBLE;
  } else if ((IK_DATE == a && IK_DATETIME == b) || (IK_DATETIME == a && IK_DATE == b)) {
    ret = IK_DATETIME;
  } else {
    ret = IK_STRING;
  }
  return ret;
}

void kind_to_column(InferredKind kind, int64_t max_len, ObFileTableColumn &col)
{
  col.nullable_ = true;
  switch (kind) {
    case IK_BOOL:
      col.type_ = ObTinyIntType;
      col.length_ = 1;
      break;
    case IK_INT:
      col.type_ = ObIntType;
      col.length_ = 8;
      break;
    case IK_DOUBLE:
      col.type_ = ObDoubleType;
      col.length_ = 8;
      break;
    case IK_DATE:
      col.type_ = ObDateType;
      col.length_ = 0;
      break;
    case IK_DATETIME:
      col.type_ = ObDateTimeType;
      col.length_ = 0;
      break;
    case IK_STRING:
    default:
      col.type_ = ObVarcharType;
      col.length_ = max_len > 0 ? max_len : 1;
      break;
  }
}

bool is_regular_file(const std::string &path)
{
  bool bret = false;
  try {
    bret = std::filesystem::is_regular_file(std::filesystem::path(path));
  } catch (...) {
    bret = false;
  }
  return bret;
}

bool wildcard_match(const char *pat, int64_t plen, const char *str, int64_t slen)
{
  while (plen > 0) {
    if ('*' == *pat) {
      while (plen > 0 && '*' == *pat) {
        ++pat;
        --plen;
      }
      if (0 == plen) {
        return true;
      }
      for (int64_t i = 0; i <= slen; ++i) {
        if (wildcard_match(pat, plen, str + i, slen - i)) {
          return true;
        }
      }
      return false;
    } else if (0 == slen) {
      return false;
    } else if ('?' == *pat || *pat == *str) {
      ++pat;
      --plen;
      ++str;
      --slen;
    } else {
      return false;
    }
  }
  return 0 == slen;
}

// ---------------- thrift compact protocol ----------------
struct CompactReader
{
  const uint8_t *b_;
  int64_t n_;
  int64_t p_;
  explicit CompactReader(const char *buf, int64_t len)
    : b_(reinterpret_cast<const uint8_t *>(buf)), n_(len), p_(0) {}
  int read_byte(uint8_t &v)
  {
    int ret = OB_SUCCESS;
    if (p_ >= n_) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      v = b_[p_++];
    }
    return ret;
  }
  int read_varint(uint64_t &v)
  {
    int ret = OB_SUCCESS;
    v = 0;
    int shift = 0;
    uint8_t x = 0;
    bool done = false;
    while (!done && OB_SUCC(ret)) {
      if (OB_FAIL(read_byte(x))) {
      } else {
        v |= (static_cast<uint64_t>(x & 0x7f)) << shift;
        if (0 == (x & 0x80)) {
          done = true;
        } else {
          shift += 7;
          if (shift > 63) {
            ret = OB_ERR_UNEXPECTED;
          }
        }
      }
    }
    return ret;
  }
  int read_zigzag(int64_t &v)
  {
    int ret = OB_SUCCESS;
    uint64_t u = 0;
    if (OB_FAIL(read_varint(u))) {
    } else {
      v = static_cast<int64_t>((u >> 1) ^ (~(u & 1) + 1));
    }
    return ret;
  }
  int read_binary(const char *&ptr, int64_t &len)
  {
    int ret = OB_SUCCESS;
    uint64_t l = 0;
    if (OB_FAIL(read_varint(l))) {
    } else if (p_ + static_cast<int64_t>(l) > n_) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ptr = reinterpret_cast<const char *>(b_ + p_);
      len = static_cast<int64_t>(l);
      p_ += l;
    }
    return ret;
  }
  int read_list_header(uint8_t &elem_type, int64_t &size)
  {
    int ret = OB_SUCCESS;
    uint8_t h = 0;
    if (OB_FAIL(read_byte(h))) {
    } else {
      elem_type = h & 0x0f;
      size = (h >> 4) & 0x0f;
      if (15 == size) {
        uint64_t u = 0;
        if (OB_FAIL(read_varint(u))) {
        } else {
          size = static_cast<int64_t>(u);
        }
      }
    }
    return ret;
  }
  int skip(uint8_t type)
  {
    int ret = OB_SUCCESS;
    switch (type) {
      case 1:  // true
      case 2:  // false
        break;
      case 3: {
        uint8_t d = 0;
        ret = read_byte(d);
        break;
      }
      case 4:
      case 5:
      case 6: {
        int64_t d = 0;
        ret = read_zigzag(d);
        break;
      }
      case 7: {
        if (p_ + 8 > n_) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          p_ += 8;
        }
        break;
      }
      case 8: {
        const char *ptr = NULL;
        int64_t len = 0;
        ret = read_binary(ptr, len);
        break;
      }
      case 9:
      case 10: {
        uint8_t et = 0;
        int64_t size = 0;
        if (OB_FAIL(read_list_header(et, size))) {
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
          ret = skip(et);
        }
        break;
      }
      case 11: {
        uint64_t size = 0;
        if (OB_FAIL(read_varint(size))) {
        } else if (size > 0) {
          uint8_t kt = 0, vt = 0;
          if (p_ + 1 > n_) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            kt = b_[p_] >> 4;
            vt = b_[p_] & 0x0f;
            ++p_;
          }
          for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
            if (OB_FAIL(skip(kt))) {
            } else {
              ret = skip(vt);
            }
          }
        }
        break;
      }
      case 12: {
        int32_t last_id = 0;
        bool stop = false;
        while (OB_SUCC(ret) && !stop) {
          uint8_t ft = 0;
          int32_t fid = 0;
          if (OB_FAIL(read_field_header(ft, fid, last_id))) {
          } else if (0 == ft) {
            stop = true;
          } else {
            ret = skip(ft);
          }
        }
        break;
      }
      default:
        ret = OB_ERR_UNEXPECTED;
        break;
    }
    return ret;
  }
  // type == 0 means STOP
  int read_field_header(uint8_t &type, int32_t &id, int32_t &last_id)
  {
    int ret = OB_SUCCESS;
    uint8_t h = 0;
    if (OB_FAIL(read_byte(h))) {
    } else if (0 == h) {
      type = 0;
      id = 0;
    } else {
      type = h & 0x0f;
      int32_t delta = (h >> 4) & 0x0f;
      if (0 == delta) {
        int64_t v = 0;
        if (OB_FAIL(read_zigzag(v))) {
        } else {
          id = static_cast<int32_t>(v);
        }
      } else {
        id = last_id + delta;
      }
      last_id = id;
    }
    return ret;
  }
};

// ---------------- snappy raw block decompress ----------------
int snappy_uncompress(const char *input, int64_t in_len, char *output, int64_t out_cap, int64_t &out_len)
{
  int ret = OB_SUCCESS;
  const uint8_t *in = reinterpret_cast<const uint8_t *>(input);
  int64_t ip = 0;
  uint64_t expected = 0;
  int shift = 0;
  bool done = false;
  while (!done && OB_SUCC(ret)) {
    if (ip >= in_len) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      uint8_t x = in[ip++];
      expected |= (static_cast<uint64_t>(x & 0x7f)) << shift;
      if (0 == (x & 0x80)) {
        done = true;
      } else {
        shift += 7;
      }
    }
  }
  int64_t op = 0;
  while (OB_SUCC(ret) && ip < in_len) {
    uint8_t tag = in[ip++];
    int64_t len = 0;
    int64_t offset = 0;
    if (0 == (tag & 0x03)) {
      len = (tag >> 2) + 1;
      if (len > 60) {
        int64_t extra = len - 60;
        len = 0;
        for (int64_t i = 0; i < extra && ip < in_len; ++i) {
          len |= static_cast<int64_t>(in[ip++]) << (8 * i);
        }
        len += 1;
      }
      if (ip + len > in_len || op + len > out_cap) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        MEMCPY(output + op, in + ip, len);
        ip += len;
        op += len;
      }
    } else {
      if (1 == (tag & 0x03)) {
        len = ((tag >> 2) & 0x07) + 4;
        if (ip >= in_len) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          offset = (static_cast<int64_t>(tag >> 5) << 8) | in[ip++];
        }
      } else if (2 == (tag & 0x03)) {
        len = (tag >> 2) + 1;
        if (ip + 2 > in_len) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          offset = static_cast<int64_t>(in[ip]) | (static_cast<int64_t>(in[ip + 1]) << 8);
          ip += 2;
        }
      } else {
        len = (tag >> 2) + 1;
        if (ip + 4 > in_len) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          offset = static_cast<int64_t>(in[ip]) | (static_cast<int64_t>(in[ip + 1]) << 8) |
                   (static_cast<int64_t>(in[ip + 2]) << 16) | (static_cast<int64_t>(in[ip + 3]) << 24);
          ip += 4;
        }
      }
      if (OB_SUCC(ret)) {
        if (offset <= 0 || offset > op || op + len > out_cap) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          for (int64_t i = 0; i < len; ++i) {
            output[op + i] = output[op - offset + i];
          }
          op += len;
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    out_len = op;
    UNUSED(expected);
  }
  return ret;
}

// ---------------- parquet meta ----------------
enum ParquetType
{
  PQT_BOOLEAN = 0,
  PQT_INT32 = 1,
  PQT_INT64 = 2,
  PQT_INT96 = 3,
  PQT_FLOAT = 4,
  PQT_DOUBLE = 5,
  PQT_BYTE_ARRAY = 6,
  PQT_FIXED_LEN_BYTE_ARRAY = 7,
};

enum ParquetCodec
{
  PQC_UNCOMPRESSED = 0,
  PQC_SNAPPY = 1,
};

struct ParquetSchemaElement
{
  int32_t type_;
  int32_t type_length_;
  int32_t repetition_;
  int32_t num_children_;
  ObString name_;
  ParquetSchemaElement()
    : type_(-1), type_length_(0), repetition_(0), num_children_(0), name_() {}
};

struct ParquetPageHeader
{
  int32_t type_;
  int32_t uncompressed_size_;
  int32_t compressed_size_;
  int32_t num_values_;
  int32_t encoding_;
  int32_t def_level_len_;
  int32_t rep_level_len_;
  bool is_compressed_;
  ParquetPageHeader()
    : type_(0), uncompressed_size_(0), compressed_size_(0), num_values_(0),
      encoding_(0), def_level_len_(0), rep_level_len_(0), is_compressed_(true) {}
};

int parse_data_page_header(CompactReader &r, ParquetPageHeader &ph, bool v2)
{
  int ret = OB_SUCCESS;
  int32_t last = 0;
  bool stop = false;
  while (OB_SUCC(ret) && !stop) {
    uint8_t type = 0;
    int32_t id = 0;
    if (OB_FAIL(r.read_field_header(type, id, last))) {
    } else if (0 == type) {
      stop = true;
    } else if (v2 && 1 == id) {
      int64_t v = 0;
      ret = r.read_zigzag(v);
      ph.num_values_ = static_cast<int32_t>(v);
    } else if (v2 && 4 == id) {
      int64_t v = 0;
      ret = r.read_zigzag(v);
      ph.encoding_ = static_cast<int32_t>(v);
    } else if (v2 && 5 == id) {
      int64_t v = 0;
      ret = r.read_zigzag(v);
      ph.def_level_len_ = static_cast<int32_t>(v);
    } else if (v2 && 6 == id) {
      int64_t v = 0;
      ret = r.read_zigzag(v);
      ph.rep_level_len_ = static_cast<int32_t>(v);
    } else if (v2 && 7 == id) {
      ph.is_compressed_ = (1 == type);
    } else if (!v2 && 1 == id) {
      int64_t v = 0;
      ret = r.read_zigzag(v);
      ph.num_values_ = static_cast<int32_t>(v);
    } else if (!v2 && 2 == id) {
      int64_t v = 0;
      ret = r.read_zigzag(v);
      ph.encoding_ = static_cast<int32_t>(v);
    } else {
      ret = r.skip(type);
    }
  }
  return ret;
}

int parse_page_header(const char *buf, int64_t len, ParquetPageHeader &ph, int64_t &consumed)
{
  int ret = OB_SUCCESS;
  CompactReader r(buf, len);
  int32_t last = 0;
  bool stop = false;
  while (OB_SUCC(ret) && !stop) {
    uint8_t type = 0;
    int32_t id = 0;
    if (OB_FAIL(r.read_field_header(type, id, last))) {
    } else if (0 == type) {
      stop = true;
    } else if (1 == id) {
      int64_t v = 0;
      ret = r.read_zigzag(v);
      ph.type_ = static_cast<int32_t>(v);
    } else if (2 == id) {
      int64_t v = 0;
      ret = r.read_zigzag(v);
      ph.uncompressed_size_ = static_cast<int32_t>(v);
    } else if (3 == id) {
      int64_t v = 0;
      ret = r.read_zigzag(v);
      ph.compressed_size_ = static_cast<int32_t>(v);
    } else if (5 == id) {
      ret = parse_data_page_header(r, ph, false);
    } else if (8 == id) {
      ret = parse_data_page_header(r, ph, true);
    } else {
      ret = r.skip(type);
    }
  }
  if (OB_SUCC(ret)) {
    consumed = r.p_;
  }
  return ret;
}

// decode RLE/bit-packed hybrid values, output one byte per value (bit width <= 8)
int decode_rle_bitpacked(const uint8_t *data, int64_t len, int64_t bit_width,
                         int64_t num_values, uint8_t *out)
{
  int ret = OB_SUCCESS;
  int64_t ip = 0;
  int64_t produced = 0;
  while (OB_SUCC(ret) && produced < num_values && ip < len) {
    uint64_t header = 0;
    int shift = 0;
    bool done = false;
    while (!done && OB_SUCC(ret)) {
      if (ip >= len) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        uint8_t x = data[ip++];
        header |= (static_cast<uint64_t>(x & 0x7f)) << shift;
        if (0 == (x & 0x80)) {
          done = true;
        } else {
          shift += 7;
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (0 == (header & 1)) {
        int64_t run = static_cast<int64_t>(header >> 1);
        int64_t bytes = (bit_width + 7) / 8;
        int64_t value = 0;
        for (int64_t i = 0; i < bytes && ip < len; ++i) {
          value |= static_cast<int64_t>(data[ip++]) << (8 * i);
        }
        for (int64_t i = 0; i < run && produced < num_values; ++i) {
          out[produced++] = static_cast<uint8_t>(value);
        }
      } else {
        int64_t groups = static_cast<int64_t>(header >> 1);
        int64_t nbytes = groups * bit_width;
        if (ip + nbytes > len) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          for (int64_t g = 0; g < groups && produced < num_values; ++g) {
            for (int64_t bit = 0; bit < 8 && produced < num_values; ++bit) {
              int64_t bit_pos = g * 8 * bit_width + bit * bit_width;
              int64_t byte_pos = bit_pos / 8;
              int64_t bit_off = bit_pos % 8;
              int64_t v = 0;
              for (int64_t k = 0; k < bit_width; ++k) {
                int64_t bp = bit_off + k;
                if (data[ip + byte_pos + bp / 8] & (1 << (bp % 8))) {
                  v |= (1 << k);
                }
              }
              out[produced++] = static_cast<uint8_t>(v);
            }
          }
          ip += nbytes;
        }
      }
    }
  }
  if (OB_SUCC(ret) && produced < num_values) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

int32_t read_le32(const uint8_t *p)
{
  return static_cast<int32_t>(p[0]) | (static_cast<int32_t>(p[1]) << 8) |
         (static_cast<int32_t>(p[2]) << 16) | (static_cast<int32_t>(p[3]) << 24);
}

int64_t read_le64(const uint8_t *p)
{
  return static_cast<int64_t>(p[0]) | (static_cast<int64_t>(p[1]) << 8) |
         (static_cast<int64_t>(p[2]) << 16) | (static_cast<int64_t>(p[3]) << 24) |
         (static_cast<int64_t>(p[4]) << 32) | (static_cast<int64_t>(p[5]) << 40) |
         (static_cast<int64_t>(p[6]) << 48) | (static_cast<int64_t>(p[7]) << 56);
}

struct TextArena
{
  ObIAllocator *alloc_;
  char *buf_;
  int64_t cap_;
  int64_t len_;
  TextArena() : alloc_(NULL), buf_(NULL), cap_(0), len_(0) {}
  int append(const char *s, int64_t n, ObString &out)
  {
    int ret = OB_SUCCESS;
    if (len_ + n + 1 > cap_) {
      int64_t new_cap = cap_ > 0 ? cap_ * 2 : 4096;
      while (new_cap < len_ + n + 1) {
        new_cap *= 2;
      }
      char *new_buf = static_cast<char *>(alloc_->alloc(new_cap));
      if (NULL == new_buf) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        if (len_ > 0) {
          MEMCPY(new_buf, buf_, len_);
        }
        buf_ = new_buf;
        cap_ = new_cap;
      }
    }
    if (OB_SUCC(ret)) {
      if (n > 0) {
        MEMCPY(buf_ + len_, s, n);
      }
      buf_[len_ + n] = '\0';
      out.assign_ptr(buf_ + len_, static_cast<int32_t>(n));
      len_ += n + 1;
    }
    return ret;
  }
  int append_int(int64_t v, ObString &out)
  {
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%lld", static_cast<long long>(v));
    return append(tmp, n, out);
  }
  int append_double(double v, ObString &out)
  {
    char tmp[64];
    int n = snprintf(tmp, sizeof(tmp), "%g", v);
    return append(tmp, n, out);
  }
};

int decode_plain_value(int32_t ptype, int32_t type_length,
                       const uint8_t *data, int64_t len,
                       int64_t &pos, TextArena &arena, ObString &text)
{
  int ret = OB_SUCCESS;
  switch (ptype) {
    case PQT_BOOLEAN: {
      int64_t byte_pos = pos / 8;
      int64_t bit = 7 - (pos % 8);
      if (byte_pos >= len) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        bool v = 0 != ((data[byte_pos] >> bit) & 1);
        ret = arena.append(v ? "true" : "false", v ? 4 : 5, text);
      }
      ++pos;
      break;
    }
    case PQT_INT32: {
      if (pos + 4 > len) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ret = arena.append_int(read_le32(data + pos), text);
        pos += 4;
      }
      break;
    }
    case PQT_INT64: {
      if (pos + 8 > len) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ret = arena.append_int(read_le64(data + pos), text);
        pos += 8;
      }
      break;
    }
    case PQT_FLOAT: {
      if (pos + 4 > len) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        float v = 0.0;
        MEMCPY(&v, data + pos, 4);
        ret = arena.append_double(static_cast<double>(v), text);
        pos += 4;
      }
      break;
    }
    case PQT_DOUBLE: {
      if (pos + 8 > len) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        double v = 0.0;
        MEMCPY(&v, data + pos, 8);
        ret = arena.append_double(v, text);
        pos += 8;
      }
      break;
    }
    case PQT_BYTE_ARRAY: {
      if (pos + 4 > len) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        int32_t l = read_le32(data + pos);
        pos += 4;
        if (l < 0 || pos + l > len) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          ret = arena.append(reinterpret_cast<const char *>(data + pos), l, text);
          pos += l;
        }
      }
      break;
    }
    case PQT_FIXED_LEN_BYTE_ARRAY: {
      if (pos + type_length > len) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ret = arena.append(reinterpret_cast<const char *>(data + pos), type_length, text);
        pos += type_length;
      }
      break;
    }
    default:
      ret = OB_NOT_SUPPORTED;
      break;
  }
  return ret;
}

// glob matching used by FILE_TABLE_GLOB
int collect_glob_files(ObIAllocator &alloc, const ObString &pattern,
                       ObIArray<ObString> &files)
{
  int ret = OB_SUCCESS;
  std::string full(pattern.ptr(), pattern.length());
  size_t slash = full.find_last_of("/\\");
  std::string dir = (std::string::npos == slash) ? std::string(".") : full.substr(0, slash);
  std::string pat = (std::string::npos == slash) ? full : full.substr(slash + 1);
  if (dir.empty()) {
    dir = "/";
  }
  std::vector<std::string> matched;
  // a shell pattern does not match a name that starts with a dot unless the
  // pattern itself starts with one
  const bool pattern_matches_hidden = !pat.empty() && '.' == pat[0];
  try {
    for (const auto &entry : std::filesystem::directory_iterator(std::filesystem::path(dir))) {
      std::string name = entry.path().filename().string();
      if (!pattern_matches_hidden && !name.empty() && '.' == name[0]) {
        continue;
      }
      if (wildcard_match(pat.c_str(), static_cast<int64_t>(pat.size()),
                         name.c_str(), static_cast<int64_t>(name.size()))) {
        if (entry.is_regular_file()) {
          std::string p = dir;
          if (!p.empty() && '/' != p[p.size() - 1]) {
            p += "/";
          }
          p += name;
          matched.push_back(p);
        }
      }
    }
  } catch (...) {
    ret = OB_IO_ERROR;
  }
  std::sort(matched.begin(), matched.end());
  for (size_t i = 0; OB_SUCC(ret) && i < matched.size(); ++i) {
    char *buf = static_cast<char *>(alloc.alloc(static_cast<int64_t>(matched[i].size())));
    if (NULL == buf) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(buf, matched[i].c_str(), static_cast<int64_t>(matched[i].size()));
      ObString s(static_cast<int32_t>(matched[i].size()), buf);
      if (OB_FAIL(files.push_back(s))) {
      }
    }
  }
  return ret;
}

struct ParquetColumn
{
  int32_t type_;
  int32_t type_length_;
  int32_t codec_;
  int64_t offset_;
  int64_t num_values_;
  bool nullable_;
  ParquetColumn()
    : type_(-1), type_length_(0), codec_(0), offset_(0), num_values_(0), nullable_(true) {}
};

struct ParquetRowGroup
{
  int64_t num_rows_;
  int64_t begin_;
  int64_t count_;
  ParquetRowGroup() : num_rows_(0), begin_(0), count_(0) {}
};

int parse_schema_element(CompactReader &r, ParquetSchemaElement &e)
{
  int ret = OB_SUCCESS;
  int32_t last = 0;
  bool stop = false;
  while (OB_SUCC(ret) && !stop) {
    uint8_t type = 0;
    int32_t id = 0;
    if (OB_FAIL(r.read_field_header(type, id, last))) {
    } else if (0 == type) {
      stop = true;
    } else if (1 == id) {
      int64_t v = 0;
      ret = r.read_zigzag(v);
      e.type_ = static_cast<int32_t>(v);
    } else if (2 == id) {
      int64_t v = 0;
      ret = r.read_zigzag(v);
      e.type_length_ = static_cast<int32_t>(v);
    } else if (3 == id) {
      int64_t v = 0;
      ret = r.read_zigzag(v);
      e.repetition_ = static_cast<int32_t>(v);
    } else if (4 == id) {
      const char *ptr = NULL;
      int64_t len = 0;
      if (OB_FAIL(r.read_binary(ptr, len))) {
      } else {
        e.name_.assign_ptr(ptr, static_cast<int32_t>(len));
      }
    } else if (5 == id) {
      int64_t v = 0;
      ret = r.read_zigzag(v);
      e.num_children_ = static_cast<int32_t>(v);
    } else {
      ret = r.skip(type);
    }
  }
  return ret;
}

int parse_column_meta(CompactReader &r, ParquetColumn &c)
{
  int ret = OB_SUCCESS;
  int32_t last = 0;
  bool stop = false;
  while (OB_SUCC(ret) && !stop) {
    uint8_t type = 0;
    int32_t id = 0;
    if (OB_FAIL(r.read_field_header(type, id, last))) {
    } else if (0 == type) {
      stop = true;
    } else if (1 == id) {
      int64_t v = 0;
      ret = r.read_zigzag(v);
      c.type_ = static_cast<int32_t>(v);
    } else if (4 == id) {
      int64_t v = 0;
      ret = r.read_zigzag(v);
      c.codec_ = static_cast<int32_t>(v);
    } else if (5 == id) {
      int64_t v = 0;
      ret = r.read_zigzag(v);
      c.num_values_ = v;
    } else if (9 == id) {
      int64_t v = 0;
      ret = r.read_zigzag(v);
      c.offset_ = v;
    } else {
      ret = r.skip(type);
    }
  }
  return ret;
}

int parse_column_chunk(CompactReader &r, ParquetColumn &c)
{
  int ret = OB_SUCCESS;
  int32_t last = 0;
  bool stop = false;
  while (OB_SUCC(ret) && !stop) {
    uint8_t type = 0;
    int32_t id = 0;
    if (OB_FAIL(r.read_field_header(type, id, last))) {
    } else if (0 == type) {
      stop = true;
    } else if (3 == id) {
      ret = parse_column_meta(r, c);
    } else {
      ret = r.skip(type);
    }
  }
  return ret;
}

int parse_row_group(CompactReader &r, ObIArray<ParquetColumn> &cols, ParquetRowGroup &rg)
{
  int ret = OB_SUCCESS;
  int32_t last = 0;
  bool stop = false;
  while (OB_SUCC(ret) && !stop) {
    uint8_t type = 0;
    int32_t id = 0;
    if (OB_FAIL(r.read_field_header(type, id, last))) {
    } else if (0 == type) {
      stop = true;
    } else if (1 == id) {
      uint8_t et = 0;
      int64_t n = 0;
      if (OB_FAIL(r.read_list_header(et, n))) {
      } else if (12 != et) {
        ret = OB_ERR_UNEXPECTED;
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < n; ++i) {
        ParquetColumn c;
        if (OB_FAIL(parse_column_chunk(r, c))) {
        } else if (OB_FAIL(cols.push_back(c))) {
        }
      }
    } else if (3 == id) {
      int64_t v = 0;
      ret = r.read_zigzag(v);
      rg.num_rows_ = v;
    } else {
      ret = r.skip(type);
    }
  }
  return ret;
}

int parse_file_meta(CompactReader &r, ObIArray<ParquetSchemaElement> &schema,
                    int64_t &num_rows, ObIArray<ParquetColumn> &cols,
                    ObIArray<ParquetRowGroup> &rgs)
{
  int ret = OB_SUCCESS;
  int32_t last = 0;
  bool stop = false;
  while (OB_SUCC(ret) && !stop) {
    uint8_t type = 0;
    int32_t id = 0;
    if (OB_FAIL(r.read_field_header(type, id, last))) {
    } else if (0 == type) {
      stop = true;
    } else if (2 == id) {
      uint8_t et = 0;
      int64_t n = 0;
      if (OB_FAIL(r.read_list_header(et, n))) {
      } else if (12 != et) {
        ret = OB_ERR_UNEXPECTED;
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < n; ++i) {
        ParquetSchemaElement e;
        if (OB_FAIL(parse_schema_element(r, e))) {
        } else if (OB_FAIL(schema.push_back(e))) {
        }
      }
    } else if (3 == id) {
      ret = r.read_zigzag(num_rows);
    } else if (4 == id) {
      uint8_t et = 0;
      int64_t n = 0;
      if (OB_FAIL(r.read_list_header(et, n))) {
      } else if (12 != et) {
        ret = OB_ERR_UNEXPECTED;
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < n; ++i) {
        ParquetRowGroup rg;
        rg.begin_ = cols.count();
        if (OB_FAIL(parse_row_group(r, cols, rg))) {
        } else {
          rg.count_ = cols.count() - rg.begin_;
          if (OB_FAIL(rgs.push_back(rg))) {
          }
        }
      }
    } else {
      ret = r.skip(type);
    }
  }
  return ret;
}

} // anonymous namespace

struct ObFileTableReader::ObParquetCtx
{
  ObArray<ParquetColumn, ModulePageAllocator, true> cols_;
  ObArray<ParquetRowGroup, ModulePageAllocator, true> rgs_;
  ObArray<ParquetSchemaElement, ModulePageAllocator, true> schema_;
  int64_t num_cols_;
  ObParquetCtx() : cols_(), rgs_(), schema_(), num_cols_(0) {}
};

ObFileTableReader::ObFileTableReader()
  : alloc_(),
    fp_(NULL), buf_(NULL), buf_cap_(0), buf_pos_(0), buf_len_(0), eof_(false),
    header_skipped_(false), row_buf_(NULL), row_cap_(0), row_len_(0), file_type_(FILE_TABLE_INVALID),
    glob_files_(), glob_pos_(0), columns_(NULL), column_count_(0),
    pq_data_(NULL), pq_len_(0), pq_pos_(0),
    pq_rows_done_(0), pq_ctx_(NULL), pq_rg_idx_(-1), pq_rg_row_(0), pq_rg_rows_(0),
    pq_values_(), opened_(false)
{
}

ObFileTableReader::~ObFileTableReader()
{
  close();
  if (NULL != pq_ctx_) {
    pq_ctx_->~ObParquetCtx();
    pq_ctx_ = NULL;
  }
}

void ObFileTableReader::close()
{
  if (NULL != fp_) {
    fclose(fp_);
    fp_ = NULL;
  }
  opened_ = false;
}

int ObFileTableReader::ensure_row_cap(int64_t need)
{
  int ret = OB_SUCCESS;
  if (need + 1 > row_cap_) {
    int64_t new_cap = row_cap_ > 0 ? row_cap_ * 2 : 4096;
    while (new_cap < need + 1) {
      new_cap *= 2;
    }
    char *new_buf = static_cast<char *>(alloc_.alloc(new_cap));
    if (NULL == new_buf) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      if (row_len_ > 0) {
        MEMCPY(new_buf, row_buf_, row_len_);
      }
      row_buf_ = new_buf;
      row_cap_ = new_cap;
    }
  }
  return ret;
}

int ObFileTableReader::append_row_char(char c)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ensure_row_cap(row_len_ + 1))) {
  } else {
    row_buf_[row_len_++] = c;
  }
  return ret;
}

void ObFileTableReader::reset_row()
{
  row_len_ = 0;
}

int ObFileTableReader::open_file(const char *path)
{
  int ret = OB_SUCCESS;
  fp_ = fopen(path, "rb");
  if (NULL == fp_) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("failed to open file", K(ret), KCSTRING(path));
  } else if (NULL == buf_) {
    buf_cap_ = FILE_READ_BUFFER_SIZE;
    buf_ = static_cast<char *>(alloc_.alloc(buf_cap_));
    if (NULL == buf_) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to allocate read buffer", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    buf_pos_ = 0;
    buf_len_ = 0;
    eof_ = false;
  }
  return ret;
}

int ObFileTableReader::fill_buffer()
{
  int ret = OB_SUCCESS;
  if (NULL == fp_) {
    ret = OB_NOT_INIT;
  } else {
    buf_len_ = static_cast<int64_t>(fread(buf_, 1, buf_cap_, fp_));
    buf_pos_ = 0;
    if (buf_len_ <= 0) {
      buf_len_ = 0;
      eof_ = true;
    }
  }
  return ret;
}

int ObFileTableReader::read_impl_char(char &c, bool &at_eof)
{
  int ret = OB_SUCCESS;
  at_eof = false;
  if (buf_pos_ >= buf_len_) {
    if (eof_) {
      at_eof = true;
    } else if (OB_FAIL(fill_buffer())) {
    } else if (buf_pos_ >= buf_len_) {
      at_eof = true;
    }
  }
  if (OB_SUCC(ret) && !at_eof) {
    c = buf_[buf_pos_++];
  }
  return ret;
}

int ObFileTableReader::next_csv_row(ObIArray<ObString> &fields)
{
  int ret = OB_SUCCESS;
  reset_row();
  ObSEArray<int64_t, 16> starts;
  ObSEArray<int64_t, 16> lens;  // -1 means NULL
  bool finished = false;
  bool in_quote = false;
  bool any_char = false;
  bool quoted = false;
  int64_t field_start = 0;
  char c = '\0';
  while (OB_SUCC(ret) && !finished) {
    bool at_eof = false;
    if (OB_FAIL(read_impl_char(c, at_eof))) {
    } else if (at_eof) {
      if (starts.count() > 0 || any_char) {
        int64_t len = quoted ? row_len_ - field_start : row_len_ - field_start;
        if (!quoted && 0 == len) {
          len = -1;
        }
        if (OB_FAIL(starts.push_back(field_start)) || OB_FAIL(lens.push_back(len))) {
        }
      }
      finished = true;
    } else if (in_quote) {
      any_char = true;
      if ('"' == c) {
        bool next_eof = false;
        char nc = '\0';
        if (OB_FAIL(read_impl_char(nc, next_eof))) {
        } else if (next_eof) {
          in_quote = false;
          if (OB_FAIL(starts.push_back(field_start)) || OB_FAIL(lens.push_back(row_len_ - field_start))) {
          }
          finished = true;
        } else if ('"' == nc) {
          if (OB_FAIL(append_row_char('"'))) {
          }
        } else {
          in_quote = false;
          if (OB_FAIL(starts.push_back(field_start)) || OB_FAIL(lens.push_back(row_len_ - field_start))) {
          } else {
            field_start = row_len_;
            quoted = false;
            if (',' == nc) {
            } else if ('\n' == nc) {
              finished = true;
            } else if ('\r' == nc) {
            } else {
              if (OB_FAIL(append_row_char(nc))) {
              }
            }
          }
        }
      } else if (OB_FAIL(append_row_char(c))) {
      }
    } else {
      any_char = true;
      if ('"' == c) {
        in_quote = true;
        quoted = true;
      } else if (',' == c) {
        int64_t len = quoted ? (row_len_ - field_start) : (row_len_ - field_start);
        if (!quoted && 0 == len) {
          len = -1;
        }
        if (OB_FAIL(starts.push_back(field_start)) || OB_FAIL(lens.push_back(len))) {
        } else {
          field_start = row_len_;
          quoted = false;
        }
      } else if ('\n' == c) {
        int64_t len = quoted ? (row_len_ - field_start) : (row_len_ - field_start);
        if (!quoted && 0 == len) {
          len = -1;
        }
        if (OB_FAIL(starts.push_back(field_start)) || OB_FAIL(lens.push_back(len))) {
        }
        finished = true;
      } else if ('\r' == c) {
        // skip, '\n' terminates the record
      } else {
        if (OB_FAIL(append_row_char(c))) {
        }
      }
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < starts.count(); ++i) {
    ObString f;
    if (lens.at(i) >= 0) {
      f.assign_ptr(row_buf_ + starts.at(i), static_cast<int32_t>(lens.at(i)));
    } else {
      f = ObString();
    }
    if (OB_FAIL(fields.push_back(f))) {
    }
  }
  if (OB_SUCC(ret) && 0 == starts.count()) {
    ret = OB_ITER_END;
  }
  return ret;
}

int ObFileTableReader::next_json_row(ObIArray<ObString> &fields)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObString, 16> keys;
  ObSEArray<ObString, 16> values;
  ObSEArray<bool, 16> is_null;
  if (OB_FAIL(next_json_raw(keys, values, is_null))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < column_count_; ++i) {
      ObString value;
      bool found = false;
      for (int64_t j = 0; !found && j < keys.count(); ++j) {
        if (keys.at(j) == columns_[i].name_) {
          found = true;
          if (is_null.at(j)) {
            value = ObString();
          } else {
            value = values.at(j);
          }
        }
      }
      if (OB_FAIL(fields.push_back(value))) {
      }
    }
  }
  return ret;
}

int ObFileTableReader::next_json_raw(ObIArray<ObString> &keys,
                                     ObIArray<ObString> &values,
                                     ObIArray<bool> &is_null)
{
  int ret = OB_SUCCESS;
  reset_row();
  bool got_line = false;
  bool at_eof = false;
  while (OB_SUCC(ret) && !got_line) {
    char c = '\0';
    if (OB_FAIL(read_impl_char(c, at_eof))) {
    } else if (at_eof) {
      if (row_len_ > 0) {
        got_line = true;
      } else {
        ret = OB_ITER_END;
      }
    } else if ('\n' == c) {
      if (row_len_ > 0) {
        got_line = true;
      }
    } else if ('\r' == c) {
      // ignore
    } else if (OB_FAIL(append_row_char(c))) {
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(ensure_row_cap(row_len_ * 4 + 256))) {
    } else {
      ret = parse_json_line(row_buf_, row_len_, keys, values, is_null);
    }
  }
  return ret;
}


int ObFileTableReader::parse_json_line(const char *line, int64_t len,
                                       ObIArray<ObString> &keys,
                                       ObIArray<ObString> &values,
                                       ObIArray<bool> &is_null)
{
  int ret = OB_SUCCESS;
  ObSEArray<int64_t, 16> key_starts;
  ObSEArray<int64_t, 16> key_lens;
  ObSEArray<int64_t, 16> val_starts;
  ObSEArray<int64_t, 16> val_lens;
  ObSEArray<bool, 16> val_nulls;
  int64_t i = 0;
  while (i < len && isspace(static_cast<unsigned char>(line[i]))) {
    ++i;
  }
  if (i >= len || '{' != line[i]) {
    // not a json object, the whole line is ignored
  } else {
    ++i;
    bool stop = false;
    while (OB_SUCC(ret) && !stop) {
      while (i < len && isspace(static_cast<unsigned char>(line[i]))) {
        ++i;
      }
      if (i >= len || '}' == line[i]) {
        stop = true;
      } else if (',' == line[i]) {
        ++i;
      } else if ('"' != line[i]) {
        stop = true;
      } else {
        int64_t kstart = row_len_;
        int64_t klen = 0;
        // parse key
        ++i;
        bool key_done = false;
        while (OB_SUCC(ret) && !key_done) {
          if (i >= len) {
            ret = OB_ERR_UNEXPECTED;
          } else if ('\\' == line[i] && i + 1 < len) {
            if (OB_FAIL(append_row_char(line[i + 1]))) {
            } else {
              i += 2;
            }
          } else if ('"' == line[i]) {
            ++i;
            key_done = true;
          } else {
            if (OB_FAIL(append_row_char(line[i]))) {
            }
            ++i;
          }
        }
        klen = row_len_ - kstart;
        while (OB_SUCC(ret) && i < len && isspace(static_cast<unsigned char>(line[i]))) {
          ++i;
        }
        if (OB_SUCC(ret)) {
          if (i >= len || ':' != line[i]) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            ++i;
            while (i < len && isspace(static_cast<unsigned char>(line[i]))) {
              ++i;
            }
          }
        }
        bool isnull = false;
        int64_t vstart = row_len_;
        int64_t vlen = 0;
        if (OB_SUCC(ret)) {
          char vc = (i < len) ? line[i] : '\0';
          if ('"' == vc) {
            ++i;
            bool val_done = false;
            while (OB_SUCC(ret) && !val_done) {
              if (i >= len) {
                ret = OB_ERR_UNEXPECTED;
              } else if ('\\' == line[i] && i + 1 < len) {
                char esc = line[i + 1];
                char decoded = esc;
                if ('n' == esc) {
                  decoded = '\n';
                } else if ('t' == esc) {
                  decoded = '\t';
                } else if ('r' == esc) {
                  decoded = '\r';
                } else if ('b' == esc) {
                  decoded = '\b';
                } else if ('f' == esc) {
                  decoded = '\f';
                } else if ('u' == esc && i + 5 < len) {
                  // decode one utf-8 code point, only bmp is supported
                  int64_t code = 0;
                  bool hex_ok = true;
                  for (int64_t k = 0; hex_ok && k < 4; ++k) {
                    char h = line[i + 2 + k];
                    code <<= 4;
                    if (h >= '0' && h <= '9') {
                      code |= (h - '0');
                    } else if (h >= 'a' && h <= 'f') {
                      code |= (h - 'a' + 10);
                    } else if (h >= 'A' && h <= 'F') {
                      code |= (h - 'A' + 10);
                    } else {
                      hex_ok = false;
                    }
                  }
                  if (hex_ok) {
                    if (code < 0x80) {
                      if (OB_FAIL(append_row_char(static_cast<char>(code)))) {
                      }
                    } else if (code < 0x800) {
                      if (OB_FAIL(append_row_char(static_cast<char>(0xc0 | (code >> 6)))) ||
                          OB_FAIL(append_row_char(static_cast<char>(0x80 | (code & 0x3f))))) {
                      }
                    } else {
                      if (OB_FAIL(append_row_char(static_cast<char>(0xe0 | (code >> 12)))) ||
                          OB_FAIL(append_row_char(static_cast<char>(0x80 | ((code >> 6) & 0x3f)))) ||
                          OB_FAIL(append_row_char(static_cast<char>(0x80 | (code & 0x3f))))) {
                      }
                    }
                    i += 6;
                  } else {
                    if (OB_FAIL(append_row_char(esc))) {
                    }
                    i += 2;
                  }
                } else {
                  if (OB_FAIL(append_row_char(decoded))) {
                  }
                  i += 2;
                }
              } else if ('"' == line[i]) {
                ++i;
                val_done = true;
              } else {
                if (OB_FAIL(append_row_char(line[i]))) {
                }
                ++i;
              }
            }
          } else if ('t' == vc && i + 4 <= len && 0 == strncmp(line + i, "true", 4)) {
            if (OB_FAIL(append_row_char('t')) || OB_FAIL(append_row_char('r')) ||
                OB_FAIL(append_row_char('u')) || OB_FAIL(append_row_char('e'))) {
            }
            i += 4;
          } else if ('f' == vc && i + 5 <= len && 0 == strncmp(line + i, "false", 5)) {
            if (OB_FAIL(append_row_char('f')) || OB_FAIL(append_row_char('a')) ||
                OB_FAIL(append_row_char('l')) || OB_FAIL(append_row_char('s')) ||
                OB_FAIL(append_row_char('e'))) {
            }
            i += 5;
          } else if ('n' == vc && i + 4 <= len && 0 == strncmp(line + i, "null", 4)) {
            isnull = true;
            i += 4;
          } else if ('{' == vc || '[' == vc) {
            // nested value, keep the raw text
            int64_t depth = 0;
            bool val_done = false;
            while (OB_SUCC(ret) && !val_done && i < len) {
              char nc = line[i];
              if (OB_FAIL(append_row_char(nc))) {
              } else if ('"' == nc) {
                ++i;
                bool str_done = false;
                while (OB_SUCC(ret) && !str_done && i < len) {
                  if ('\\' == line[i] && i + 1 < len) {
                    if (OB_FAIL(append_row_char(line[i])) || OB_FAIL(append_row_char(line[i + 1]))) {
                    }
                    i += 2;
                  } else if ('"' == line[i]) {
                    if (OB_FAIL(append_row_char(line[i]))) {
                    }
                    ++i;
                    str_done = true;
                  } else {
                    if (OB_FAIL(append_row_char(line[i]))) {
                    }
                    ++i;
                  }
                }
              } else {
                ++i;
                if ('{' == nc || '[' == nc) {
                  ++depth;
                } else if ('}' == nc || ']' == nc) {
                  --depth;
                  if (0 == depth) {
                    val_done = true;
                  }
                }
              }
            }
            if (!val_done) {
              ret = OB_ERR_UNEXPECTED;
            }
          } else {
            // number or other literal
            bool val_done = false;
            while (OB_SUCC(ret) && !val_done && i < len) {
              char nc = line[i];
              if (',' == nc || '}' == nc || ']' == nc || isspace(static_cast<unsigned char>(nc))) {
                val_done = true;
              } else {
                if (OB_FAIL(append_row_char(nc))) {
                }
                ++i;
              }
            }
          }
        }
        if (OB_SUCC(ret)) {
          vlen = row_len_ - vstart;
          if (OB_FAIL(key_starts.push_back(kstart)) || OB_FAIL(key_lens.push_back(klen)) ||
              OB_FAIL(val_starts.push_back(vstart)) || OB_FAIL(val_lens.push_back(vlen)) ||
              OB_FAIL(val_nulls.push_back(isnull))) {
          }
        }
      }
    }
  }
  for (int64_t k = 0; OB_SUCC(ret) && k < key_starts.count(); ++k) {
    ObString key(static_cast<int32_t>(key_lens.at(k)), row_buf_ + key_starts.at(k));
    ObString value;
    if (!val_nulls.at(k)) {
      value.assign_ptr(row_buf_ + val_starts.at(k), static_cast<int32_t>(val_lens.at(k)));
    }
    if (OB_FAIL(keys.push_back(key)) || OB_FAIL(values.push_back(value)) ||
        OB_FAIL(is_null.push_back(val_nulls.at(k)))) {
    }
  }
  return ret;
}

int ObFileTableReader::next_parquet_row(ObIArray<ObString> &fields)
{
  int ret = OB_SUCCESS;
  if (NULL == pq_ctx_) {
    ret = OB_NOT_INIT;
  }
  while (OB_SUCC(ret) && pq_rg_row_ >= pq_rg_rows_) {
    if (pq_rg_idx_ + 1 >= pq_ctx_->rgs_.count()) {
      ret = OB_ITER_END;
    } else if (OB_FAIL(load_parquet_row_group(pq_rg_idx_ + 1))) {
    } else {
      ++pq_rg_idx_;
    }
  }
  if (OB_SUCC(ret)) {
    int64_t base = pq_rg_row_ * pq_ctx_->num_cols_;
    for (int64_t i = 0; OB_SUCC(ret) && i < pq_ctx_->num_cols_; ++i) {
      if (OB_FAIL(fields.push_back(pq_values_.at(base + i)))) {
      }
    }
    if (OB_SUCC(ret)) {
      ++pq_rg_row_;
    }
  }
  return ret;
}

int ObFileTableReader::next_glob_row(ObIArray<ObString> &fields)
{
  int ret = OB_SUCCESS;
  if (glob_pos_ >= glob_files_.count()) {
    ret = OB_ITER_END;
  } else {
    if (OB_FAIL(fields.push_back(glob_files_.at(glob_pos_)))) {
    } else {
      ++glob_pos_;
    }
  }
  return ret;
}

int ObFileTableReader::load_parquet_row_group(int64_t rg_idx)
{
  int ret = OB_SUCCESS;
  if (NULL == pq_ctx_) {
    ret = OB_NOT_INIT;
  } else if (rg_idx < 0 || rg_idx >= pq_ctx_->rgs_.count()) {
    ret = OB_ITER_END;
  } else {
    const ParquetRowGroup &rg = pq_ctx_->rgs_.at(rg_idx);
    const int64_t rows = rg.num_rows_;
    const int64_t ncols = pq_ctx_->num_cols_;
    pq_values_.reuse();
    for (int64_t i = 0; OB_SUCC(ret) && i < rows * ncols; ++i) {
      if (OB_FAIL(pq_values_.push_back(ObString()))) {
      }
    }
    TextArena arena;
    arena.alloc_ = &alloc_;
    for (int64_t c = 0; OB_SUCC(ret) && c < ncols && c < rg.count_; ++c) {
      const ParquetColumn &col = pq_ctx_->cols_.at(rg.begin_ + c);
      int64_t pos = col.offset_;
      int64_t produced = 0;
      while (OB_SUCC(ret) && produced < col.num_values_ && produced < rows) {
        ParquetPageHeader ph;
        int64_t consumed = 0;
        if (pos < 0 || pos >= pq_len_) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("invalid parquet page offset", K(ret), K(pos), K(pq_len_));
        } else if (OB_FAIL(parse_page_header(pq_data_ + pos, pq_len_ - pos, ph, consumed))) {
          LOG_WARN("failed to parse parquet page header", K(ret));
        } else if (pos + consumed + ph.compressed_size_ > pq_len_) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("invalid parquet page size", K(ret), K(pos), K(consumed), K(ph.compressed_size_));
        } else {
          const uint8_t *pdata = reinterpret_cast<const uint8_t *>(pq_data_ + pos + consumed);
          pos += consumed + ph.compressed_size_;
          if (0 != ph.type_ && 3 != ph.type_) {
            // dictionary page or index page, skip it
            continue;
          }
          const int64_t nvals = ph.num_values_;
          const uint8_t *def = NULL;
          int64_t def_len = 0;
          const uint8_t *valptr = NULL;
          int64_t vallen = 0;
          char *tmp = NULL;
          if (3 == ph.type_) {
            const int64_t rep_len = ph.rep_level_len_;
            const int64_t dfl = ph.def_level_len_;
            if (rep_len + dfl > ph.compressed_size_) {
              ret = OB_ERR_UNEXPECTED;
            } else {
              def = pdata + rep_len;
              def_len = dfl;
              valptr = pdata + rep_len + dfl;
              vallen = ph.compressed_size_ - rep_len - dfl;
              if (ph.is_compressed_ && PQC_UNCOMPRESSED != col.codec_) {
                int64_t out_len = 0;
                tmp = static_cast<char *>(alloc_.alloc(ph.uncompressed_size_));
                if (NULL == tmp) {
                  ret = OB_ALLOCATE_MEMORY_FAILED;
                } else if (PQC_SNAPPY == col.codec_) {
                  ret = snappy_uncompress(reinterpret_cast<const char *>(valptr), vallen,
                                          tmp, ph.uncompressed_size_, out_len);
                } else {
                  ret = OB_NOT_SUPPORTED;
                }
                if (OB_SUCC(ret)) {
                  valptr = reinterpret_cast<const uint8_t *>(tmp);
                  vallen = out_len;
                }
              }
            }
          } else {
            const uint8_t *page = pdata;
            int64_t plen = ph.compressed_size_;
            if (PQC_UNCOMPRESSED != col.codec_) {
              int64_t out_len = 0;
              tmp = static_cast<char *>(alloc_.alloc(ph.uncompressed_size_));
              if (NULL == tmp) {
                ret = OB_ALLOCATE_MEMORY_FAILED;
              } else if (PQC_SNAPPY == col.codec_) {
                ret = snappy_uncompress(reinterpret_cast<const char *>(pdata), plen,
                                        tmp, ph.uncompressed_size_, out_len);
              } else {
                ret = OB_NOT_SUPPORTED;
              }
              if (OB_SUCC(ret)) {
                page = reinterpret_cast<const uint8_t *>(tmp);
                plen = out_len;
              }
            }
            if (OB_SUCC(ret)) {
              if (plen < 8) {
                ret = OB_ERR_UNEXPECTED;
              } else {
                int64_t offset = 0;
                int32_t rlen = read_le32(page);
                offset += 4 + rlen;
                if (offset + 4 > plen) {
                  ret = OB_ERR_UNEXPECTED;
                } else {
                  int32_t dlen = read_le32(page + offset);
                  offset += 4;
                  def = page + offset;
                  def_len = dlen;
                  offset += dlen;
                  valptr = page + offset;
                  vallen = plen - offset;
                }
              }
            }
          }
          uint8_t *levels = NULL;
          if (OB_SUCC(ret) && def_len > 0 && NULL != def) {
            levels = static_cast<uint8_t *>(alloc_.alloc(nvals > 0 ? nvals : 1));
            if (NULL == levels) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else {
              ret = decode_rle_bitpacked(def, def_len, 1, nvals, levels);
            }
          }
          int64_t vp = 0;
          for (int64_t k = 0; OB_SUCC(ret) && k < nvals && produced < rows; ++k) {
            bool defined = (NULL == levels) ? true : (levels[k] != 0);
            ObString text;
            if (defined) {
              if (OB_FAIL(decode_plain_value(pq_ctx_->cols_.at(rg.begin_ + c).type_,
                                             pq_ctx_->cols_.at(rg.begin_ + c).type_length_,
                                             valptr, vallen, vp, arena, text))) {
                LOG_WARN("failed to decode parquet value", K(ret), K(col.type_));
              }
            }
            if (OB_SUCC(ret)) {
              pq_values_.at(c * rows + produced) = text;
              ++produced;
            }
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      pq_rg_row_ = 0;
      pq_rg_rows_ = rows;
    }
  }
  return ret;
}

static void copy_string(ObIAllocator &alloc, const ObString &src, ObString &dst)
{
  char *buf = NULL;
  dst.reset();
  if (src.length() > 0) {
    buf = static_cast<char *>(alloc.alloc(src.length()));
    if (NULL != buf) {
      MEMCPY(buf, src.ptr(), src.length());
      dst.assign_ptr(buf, src.length());
    }
  }
}

static ObObjType parquet_type_to_obj(int32_t ptype)
{
  ObObjType type = ObUnknownType;
  switch (ptype) {
    case PQT_BOOLEAN: type = ObTinyIntType; break;
    case PQT_INT32: type = ObInt32Type; break;
    case PQT_INT64: type = ObIntType; break;
    case PQT_FLOAT: type = ObFloatType; break;
    case PQT_DOUBLE: type = ObDoubleType; break;
    case PQT_BYTE_ARRAY:
    case PQT_FIXED_LEN_BYTE_ARRAY: type = ObVarcharType; break;
    default: type = ObUnknownType; break;
  }
  return type;
}

static int alloc_file_columns(ObIAllocator &alloc, int64_t n, ObFileTableColumn *&cols)
{
  int ret = OB_SUCCESS;
  cols = static_cast<ObFileTableColumn *>(alloc.alloc(sizeof(ObFileTableColumn) * (n > 0 ? n : 1)));
  if (NULL == cols) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    for (int64_t i = 0; i < n; ++i) {
      new (&cols[i]) ObFileTableColumn();
    }
  }
  return ret;
}

int ObFileTableReader::load_parquet_file(const char *path)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(open_file(path))) {
  } else if (0 != fseek(fp_, 0, SEEK_END)) {
    ret = OB_IO_ERROR;
  } else {
    long sz = ftell(fp_);
    if (sz < 0) {
      ret = OB_IO_ERROR;
    } else if (0 != fseek(fp_, 0, SEEK_SET)) {
      ret = OB_IO_ERROR;
    } else {
      pq_len_ = static_cast<int64_t>(sz);
      pq_data_ = static_cast<char *>(alloc_.alloc(pq_len_ > 0 ? pq_len_ : 1));
      if (NULL == pq_data_) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (pq_len_ > 0 &&
                 static_cast<int64_t>(fread(pq_data_, 1, pq_len_, fp_)) != pq_len_) {
        ret = OB_IO_ERROR;
      }
    }
  }
  if (NULL != fp_) {
    fclose(fp_);
    fp_ = NULL;
  }
  if (OB_SUCC(ret)) {
    if (pq_len_ < 12 || 0 != memcmp(pq_data_, "PAR1", 4) ||
        0 != memcmp(pq_data_ + pq_len_ - 4, "PAR1", 4)) {
      ret = OB_INVALID_DATA;
      LOG_WARN("invalid parquet file", K(ret), K(pq_len_));
    } else {
      const int32_t flen = read_le32(reinterpret_cast<const uint8_t *>(pq_data_) + pq_len_ - 8);
      const int64_t foff = pq_len_ - 8 - flen;
      if (flen <= 0 || foff < 4) {
        ret = OB_INVALID_DATA;
      } else {
        pq_ctx_ = new (alloc_.alloc(sizeof(ObParquetCtx))) ObParquetCtx();
        if (NULL == pq_ctx_) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          CompactReader r(pq_data_ + foff, flen);
          int64_t num_rows = 0;
          if (OB_FAIL(parse_file_meta(r, pq_ctx_->schema_, num_rows, pq_ctx_->cols_, pq_ctx_->rgs_))) {
            LOG_WARN("failed to parse parquet footer", K(ret));
          } else {
            pq_ctx_->num_cols_ = pq_ctx_->schema_.count() > 1 ? pq_ctx_->schema_.count() - 1 : 0;
            pq_rg_idx_ = -1;
            pq_rg_row_ = 0;
            pq_rg_rows_ = 0;
          }
        }
      }
    }
  }
  return ret;
}

int ObFileTableReader::open(ObIAllocator &alloc, const ObFileTableDef &def)
{
  int ret = OB_SUCCESS;
  columns_ = def.columns_;
  column_count_ = def.column_count_;
  file_type_ = def.file_type_;
  char *path = static_cast<char *>(alloc.alloc(def.path_.length() + 1));
  if (NULL == path) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    if (def.path_.length() > 0) {
      MEMCPY(path, def.path_.ptr(), def.path_.length());
    }
    path[def.path_.length()] = '\0';
    if (FILE_TABLE_CSV == def.file_type_) {
      if (OB_FAIL(open_file(path))) {
      } else if (def.has_header_ && !header_skipped_) {
        ObSEArray<ObString, 16> tmp;
        int tmp_ret = next_csv_row(tmp);
        if (OB_FAIL(tmp_ret) && OB_ITER_END != tmp_ret) {
          ret = tmp_ret;
        } else {
          header_skipped_ = true;
        }
      }
    } else if (FILE_TABLE_JSON == def.file_type_) {
      ret = open_file(path);
    } else if (FILE_TABLE_GLOB == def.file_type_) {
      ret = collect_glob_files(alloc, def.path_, glob_files_);
    } else if (FILE_TABLE_PARQUET == def.file_type_) {
      ret = load_parquet_file(path);
    } else {
      ret = OB_NOT_SUPPORTED;
    }
  }
  if (OB_SUCC(ret)) {
    opened_ = true;
  }
  return ret;
}

int ObFileTableReader::get_next_row(ObIArray<ObString> &fields)
{
  int ret = OB_SUCCESS;
  if (!opened_) {
    ret = OB_NOT_INIT;
  } else {
    switch (file_type_) {
      case FILE_TABLE_CSV:
        ret = next_csv_row(fields);
        break;
      case FILE_TABLE_JSON:
        ret = next_json_row(fields);
        break;
      case FILE_TABLE_PARQUET:
        ret = next_parquet_row(fields);
        break;
      case FILE_TABLE_GLOB:
        ret = next_glob_row(fields);
        break;
      default:
        ret = OB_NOT_SUPPORTED;
        break;
    }
  }
  return ret;
}

static int make_unique_name(ObIAllocator &alloc, const ObString &base,
                            ObIArray<ObString> &names, ObString &out)
{
  int ret = OB_SUCCESS;
  bool conflict = true;
  int64_t dup = 1;
  while (OB_SUCC(ret) && conflict) {
    conflict = false;
    char suffix[32];
    int slen = 0;
    if (dup > 1) {
      slen = snprintf(suffix, sizeof(suffix), "__%lld", static_cast<long long>(dup));
    }
    char *buf = static_cast<char *>(alloc.alloc(base.length() + slen + 1));
    if (NULL == buf) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      if (base.length() > 0) {
        MEMCPY(buf, base.ptr(), base.length());
      }
      if (slen > 0) {
        MEMCPY(buf + base.length(), suffix, slen);
      }
      buf[base.length() + slen] = '\0';
      ObString cand(base.length() + slen, buf);
      for (int64_t i = 0; !conflict && i < names.count(); ++i) {
        if (names.at(i) == cand) {
          conflict = true;
        }
      }
      if (!conflict) {
        out = cand;
      }
      ++dup;
    }
  }
  return ret;
}

int ObFileTableReader::infer_schema(ObIAllocator &alloc, ObFileTableDef &def)
{
  int ret = OB_SUCCESS;
  ObFileTableReader reader;
  char *path = static_cast<char *>(alloc.alloc(def.path_.length() + 1));
  if (NULL == path) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    if (def.path_.length() > 0) {
      MEMCPY(path, def.path_.ptr(), def.path_.length());
    }
    path[def.path_.length()] = '\0';
    if (FILE_TABLE_GLOB == def.file_type_) {
      ObFileTableColumn *cols = NULL;
      if (OB_FAIL(alloc_file_columns(alloc, 1, cols))) {
      } else {
        copy_string(alloc, ObString::make_string("file"), cols[0].name_);
        cols[0].type_ = ObVarcharType;
        cols[0].length_ = 1024;
        cols[0].nullable_ = true;
        def.columns_ = cols;
        def.column_count_ = 1;
      }
    } else if (FILE_TABLE_PARQUET == def.file_type_) {
      if (OB_FAIL(reader.load_parquet_file(path))) {
      } else if (NULL == reader.pq_ctx_) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        const int64_t total = reader.pq_ctx_->schema_.count();
        const int64_t n = total > 1 ? total - 1 : 0;
        ObFileTableColumn *cols = NULL;
        if (OB_FAIL(alloc_file_columns(alloc, n, cols))) {
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < n; ++i) {
            const ParquetSchemaElement &e = reader.pq_ctx_->schema_.at(i + 1);
            copy_string(alloc, e.name_, cols[i].name_);
            cols[i].type_ = parquet_type_to_obj(e.type_);
            cols[i].length_ = (ObVarcharType == cols[i].type_) ? 65535
                            : (ObInt32Type == cols[i].type_ ? 4 : 8);
            cols[i].nullable_ = (0 != e.repetition_);
            if (ObUnknownType == cols[i].type_) {
              ret = OB_NOT_SUPPORTED;
              LOG_WARN("unsupported parquet column type", K(ret), K(e.type_));
            }
          }
          if (OB_SUCC(ret)) {
            def.columns_ = cols;
            def.column_count_ = n;
          }
        }
      }
    } else if (FILE_TABLE_CSV == def.file_type_) {
      ObSEArray<ObString, 32> fields;
      int tmp_ret = OB_SUCCESS;
      if (OB_FAIL(reader.open_file(path))) {
      } else if (OB_ITER_END == (tmp_ret = reader.next_csv_row(fields))) {
        def.columns_ = NULL;
        def.column_count_ = 0;
      } else if (OB_FAIL(tmp_ret)) {
        ret = tmp_ret;
      } else {
        const int64_t n = fields.count();
        ObSEArray<ObString, 32> names;
        ObSEArray<int64_t, 32> kinds;
        ObSEArray<int64_t, 32> maxlens;
        ObSEArray<bool, 32> nulls;
        for (int64_t i = 0; OB_SUCC(ret) && i < n; ++i) {
          ObString raw;
          if (def.has_header_) {
            raw = fields.at(i);
          }
          const char *p = raw.ptr();
          int64_t l = raw.length();
          if (NULL == p) {
            p = "";
            l = 0;
          }
          while (l > 0 && isspace(static_cast<unsigned char>(*p))) {
            ++p;
            --l;
          }
          while (l > 0 && isspace(static_cast<unsigned char>(p[l - 1]))) {
            --l;
          }
          char tmp_name[64];
          ObString base;
          if (0 == l) {
            int m = snprintf(tmp_name, sizeof(tmp_name), "column%lld", static_cast<long long>(i + 1));
            base.assign_ptr(tmp_name, m);
          } else {
            base.assign_ptr(p, static_cast<int32_t>(l));
          }
          ObString unique_name;
          if (OB_FAIL(make_unique_name(alloc, base, names, unique_name))) {
          } else if (OB_FAIL(names.push_back(unique_name)) ||
                     OB_FAIL(kinds.push_back(IK_UNKNOWN)) ||
                     OB_FAIL(maxlens.push_back(0)) ||
                     OB_FAIL(nulls.push_back(false))) {
          }
        }
        int64_t sampled = 0;
        bool eof = false;
        while (OB_SUCC(ret) && !eof) {
          if (def.sample_size_ >= 0 && sampled >= def.sample_size_) {
            break;
          }
          ObSEArray<ObString, 32> row_fields;
          tmp_ret = reader.next_csv_row(row_fields);
          if (OB_ITER_END == tmp_ret) {
            eof = true;
          } else if (OB_FAIL(tmp_ret)) {
            ret = tmp_ret;
          } else {
            for (int64_t i = 0; OB_SUCC(ret) && i < n; ++i) {
              if (i >= row_fields.count() || NULL == row_fields.at(i).ptr()) {
                // null value, type is not affected
                nulls.at(i) = true;
              } else {
                InferredKind kind = detect_kind(row_fields.at(i).ptr(), row_fields.at(i).length());
                kinds.at(i) = merge_kind(static_cast<InferredKind>(kinds.at(i)), kind);
                if (row_fields.at(i).length() > maxlens.at(i)) {
                  maxlens.at(i) = row_fields.at(i).length();
                }
              }
            }
            ++sampled;
          }
        }
        if (OB_SUCC(ret)) {
          ObFileTableColumn *cols = NULL;
          if (OB_FAIL(alloc_file_columns(alloc, n, cols))) {
          } else {
            for (int64_t i = 0; i < n; ++i) {
              cols[i].name_ = names.at(i);
              kind_to_column(static_cast<InferredKind>(kinds.at(i)), maxlens.at(i), cols[i]);
              cols[i].nullable_ = nulls.at(i);
            }
            def.columns_ = cols;
            def.column_count_ = n;
          }
        }
      }
    } else if (FILE_TABLE_JSON == def.file_type_) {
      if (OB_FAIL(reader.open_file(path))) {
      } else {
        ObSEArray<ObString, 32> names;
        ObSEArray<int64_t, 32> kinds;
        ObSEArray<int64_t, 32> maxlens;
        bool eof = false;
        while (OB_SUCC(ret) && !eof) {
          ObSEArray<ObString, 32> keys;
          ObSEArray<ObString, 32> values;
          ObSEArray<bool, 32> is_null;
          int tmp_ret = reader.next_json_raw(keys, values, is_null);
          if (OB_ITER_END == tmp_ret) {
            eof = true;
          } else if (OB_FAIL(tmp_ret)) {
            ret = tmp_ret;
          } else {
            for (int64_t j = 0; OB_SUCC(ret) && j < keys.count(); ++j) {
              int64_t idx = -1;
              for (int64_t k = 0; -1 == idx && k < names.count(); ++k) {
                if (names.at(k) == keys.at(j)) {
                  idx = k;
                }
              }
              if (-1 == idx) {
                ObString name;
                copy_string(alloc, keys.at(j), name);
                if (OB_FAIL(names.push_back(name)) || OB_FAIL(kinds.push_back(IK_UNKNOWN)) ||
                    OB_FAIL(maxlens.push_back(0))) {
                } else {
                  idx = names.count() - 1;
                }
              }
              if (OB_SUCC(ret) && !is_null.at(j)) {
                InferredKind kind = detect_kind(values.at(j).ptr(), values.at(j).length());
                kinds.at(idx) = merge_kind(static_cast<InferredKind>(kinds.at(idx)), kind);
                if (values.at(j).length() > maxlens.at(idx)) {
                  maxlens.at(idx) = values.at(j).length();
                }
              }
            }
          }
        }
        if (OB_SUCC(ret)) {
          const int64_t n = names.count();
          ObFileTableColumn *cols = NULL;
          if (OB_FAIL(alloc_file_columns(alloc, n, cols))) {
          } else {
            for (int64_t i = 0; i < n; ++i) {
              cols[i].name_ = names.at(i);
              kind_to_column(static_cast<InferredKind>(kinds.at(i)), maxlens.at(i), cols[i]);
              cols[i].nullable_ = true;
            }
            def.columns_ = cols;
            def.column_count_ = n;
          }
        }
      }
    } else {
      ret = OB_NOT_SUPPORTED;
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
