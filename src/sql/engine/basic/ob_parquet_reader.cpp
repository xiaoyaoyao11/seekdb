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

/**
 * @file ob_parquet_reader.cpp
 *
 * Implementation of the Parquet reader.  See ob_parquet_reader.h for the
 * contract.
 */

#include "sql/engine/basic/ob_parquet_reader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

#if defined(_WIN32)
# include <filesystem>
# include <windows.h>
#else
# include <sys/stat.h>
# include <unistd.h>
#endif

namespace oceanbase
{
namespace sql
{
namespace file_scan
{

namespace
{

/// The magic number that opens and closes every Parquet file.
const char PARQUET_MAGIC[4] = {'P', 'A', 'R', '1'};

// ---------------------------------------------------------------------------
// The Thrift compact protocol
// ---------------------------------------------------------------------------

/// The Thrift compact protocol type ids.
enum ThriftType
{
  T_STOP           = 0x00,
  T_TRUE           = 0x01,
  T_FALSE          = 0x02,
  T_BYTE           = 0x03,
  T_I16            = 0x04,
  T_I32            = 0x05,
  T_I64            = 0x06,
  T_DOUBLE         = 0x07,
  T_BINARY         = 0x08,
  T_LIST           = 0x09,
  T_SET            = 0x0A,
  T_MAP            = 0x0B,
  T_STRUCT         = 0x0C,
};

/// A bounded cursor over a byte range with a Thrift compact decoder.
class CompactReader
{
public:
  CompactReader(const char *data, size_t size) : data_(data), size_(size), pos_(0) {}

  bool eof() const { return pos_ >= size_; }
  size_t pos() const { return pos_; }
  size_t size() const { return size_; }
  const char *data() const { return data_; }
  void seek(size_t pos) { pos_ = pos; }

  bool read_byte(uint8_t *out)
  {
    if (pos_ >= size_) {
      return false;
    }
    *out = static_cast<uint8_t>(data_[pos_++]);
    return true;
  }

  /// Read a varint (base 128, little endian).
  bool read_varint(uint64_t *out)
  {
    uint64_t result = 0;
    int shift       = 0;
    for (int i = 0; i < 10; ++i) {
      uint8_t byte = 0;
      if (!read_byte(&byte)) {
        return false;
      }
      result |= static_cast<uint64_t>(byte & 0x7F) << shift;
      if (0 == (byte & 0x80)) {
        *out = result;
        return true;
      }
      shift += 7;
    }
    return false;
  }

  /// Read a zigzag encoded varint.
  bool read_zigzag(int64_t *out)
  {
    uint64_t raw = 0;
    if (!read_varint(&raw)) {
      return false;
    }
    *out = static_cast<int64_t>((raw >> 1) ^ (~(raw & 1) + 1));
    return true;
  }

  /// Read a fixed width little endian integer.
  bool read_fixed(int32_t width, uint64_t *out)
  {
    if (pos_ + static_cast<size_t>(width) > size_) {
      return false;
    }
    uint64_t result = 0;
    for (int i = 0; i < width; ++i) {
      result |= static_cast<uint64_t>(static_cast<uint8_t>(data_[pos_ + i])) << (8 * i);
    }
    pos_ += static_cast<size_t>(width);
    *out = result;
    return true;
  }

  /// Read a binary (or string) value.
  bool read_binary(std::string *out)
  {
    uint64_t length = 0;
    if (!read_varint(&length)) {
      return false;
    }
    if (length > size_ - pos_ || pos_ > size_) {
      return false;
    }
    out->assign(data_ + pos_, static_cast<size_t>(length));
    pos_ += static_cast<size_t>(length);
    return true;
  }

  /// Read the header of a list or a set.
  ///
  /// The high nibble of the header holds the number of elements, the value
  /// fifteen means that the real number follows as a varint.
  bool read_list_header(uint32_t *size, uint8_t *element_type)
  {
    uint8_t header = 0;
    if (!read_byte(&header)) {
      return false;
    }
    const uint8_t type = header & 0x0F;
    uint64_t count    = 0;
    if (type == 0x0F) {
      if (!read_varint(&count)) {
        return false;
      }
    } else {
      count = static_cast<uint64_t>(header >> 4);
    }
    if (count > size_ - pos_ + 1) {
      // A list never holds more elements than the remaining bytes, this
      // rejects a corrupt length early.
      return false;
    }
    if (count > 0xFFFFFFFFULL) {
      return false;
    }
    *size         = static_cast<uint32_t>(count);
    *element_type = type;
    return true;
  }

  /// Read the header of a struct field.
  ///
  /// `field_id` receives the delta against the id of the previous field when
  /// `is_delta` is true, and the absolute id otherwise.  A delta of zero is
  /// not a valid delta, the protocol writes the absolute id in that case.
  bool read_field_header(uint8_t *type, int16_t *field_id, bool *is_delta)
  {
    uint8_t header = 0;
    if (!read_byte(&header)) {
      return false;
    }
    if (T_STOP == (header & 0x0F)) {
      *type = T_STOP;
      return true;
    }
    *type = header & 0x0F;
    const uint8_t delta = (header >> 4) & 0x0F;
    if (0 == delta) {
      int64_t id = 0;
      if (!read_zigzag(&id)) {
        return false;
      }
      *field_id = static_cast<int16_t>(id);
      *is_delta = false;
    } else {
      *field_id = static_cast<int16_t>(delta);
      *is_delta = true;
    }
    return true;
  }

private:
  const char *data_;
  size_t size_;
  size_t pos_;
};

/// A helper that keeps the last field id of a struct.
class StructCursor
{
public:
  explicit StructCursor(CompactReader &reader) : reader_(reader), last_id_(0), done_(false) {}

  /// Advance to the next field.  Returns false at the stop field.
  bool next(uint8_t *type, int16_t *field_id)
  {
    if (done_) {
      return false;
    }
    uint8_t header_type = 0;
    int16_t header_id   = 0;
    bool is_delta       = false;
    if (!reader_.read_field_header(&header_type, &header_id, &is_delta)) {
      done_ = true;
      return false;
    }
    if (T_STOP == header_type) {
      done_ = true;
      return false;
    }
    // The compact protocol encodes the field id as a delta against the id of
    // the previous field, the delta zero means that the id is written out in
    // full as a zigzag varint.
    if (is_delta) {
      last_id_ = static_cast<int16_t>(last_id_ + header_id);
    } else {
      last_id_ = header_id;
    }
    *type     = header_type;
    *field_id = last_id_;
    return true;
  }

private:
  CompactReader &reader_;
  int16_t last_id_;
  bool done_;
};

/// Skip a Thrift value of the given type.
Status skip_value(CompactReader &reader, uint8_t type, Error *error);

/// Skip the fields of a struct, the reader stops at the stop field.
Status skip_struct(CompactReader &reader, Error *error)
{
  StructCursor cursor(reader);
  uint8_t type     = 0;
  int16_t field_id = 0;
  while (cursor.next(&type, &field_id)) {
    const Status status = skip_value(reader, type, error);
    if (Status::OK != status) {
      return status;
    }
  }
  return Status::OK;
}

/// Skip a Thrift value of the given type.
Status skip_value(CompactReader &reader, uint8_t type, Error *error)
{
  switch (type) {
  case T_TRUE:
  case T_FALSE:
    // A bool carries its value in the type of the field header.
    return Status::OK;
  case T_BYTE: {
    uint8_t value = 0;
    if (!reader.read_byte(&value)) {
      error->set(Status::PARSE_ERROR, "malformed parquet footer: a value is truncated");
      return Status::PARSE_ERROR;
    }
    return Status::OK;
  }
  case T_I16:
  case T_I32:
  case T_I64: {
    // The integers of the compact protocol are zigzag encoded varints, only
    // the double has a fixed width.
    int64_t value = 0;
    if (!reader.read_zigzag(&value)) {
      error->set(Status::PARSE_ERROR, "malformed parquet footer: an integer is truncated");
      return Status::PARSE_ERROR;
    }
    return Status::OK;
  }
  case T_DOUBLE: {
    uint64_t value = 0;
    if (!reader.read_fixed(8, &value)) {
      error->set(Status::PARSE_ERROR, "malformed parquet footer: a value is truncated");
      return Status::PARSE_ERROR;
    }
    return Status::OK;
  }
  case T_BINARY: {
    std::string value;
    if (!reader.read_binary(&value)) {
      error->set(Status::PARSE_ERROR, "malformed parquet footer: a value is truncated");
      return Status::PARSE_ERROR;
    }
    return Status::OK;
  }
  case T_LIST:
  case T_SET: {
    uint32_t size = 0;
    uint8_t element_type = 0;
    if (!reader.read_list_header(&size, &element_type)) {
      error->set(Status::PARSE_ERROR, "malformed parquet footer: a list is truncated");
      return Status::PARSE_ERROR;
    }
    for (uint32_t i = 0; i < size; ++i) {
      const Status status = skip_value(reader, element_type, error);
      if (Status::OK != status) {
        return status;
      }
    }
    return Status::OK;
  }
  case T_MAP: {
    uint64_t size = 0;
    if (!reader.read_varint(&size)) {
      error->set(Status::PARSE_ERROR, "malformed parquet footer: a map is truncated");
      return Status::PARSE_ERROR;
    }
    if (0 == size) {
      return Status::OK;
    }
    uint8_t types = 0;
    if (!reader.read_byte(&types)) {
      error->set(Status::PARSE_ERROR, "malformed parquet footer: a map is truncated");
      return Status::PARSE_ERROR;
    }
    const uint8_t key_type   = (types >> 4) & 0x0F;
    const uint8_t value_type = types & 0x0F;
    for (uint64_t i = 0; i < size; ++i) {
      Status status = skip_value(reader, key_type, error);
      if (Status::OK != status) {
        return status;
      }
      status = skip_value(reader, value_type, error);
      if (Status::OK != status) {
        return status;
      }
    }
    return Status::OK;
  }
  case T_STRUCT:
    return skip_struct(reader, error);
  default:
    error->set(Status::PARSE_ERROR, "malformed parquet footer: an unknown value type was found");
    return Status::PARSE_ERROR;
  }
}

// ---------------------------------------------------------------------------
// The Parquet enumerations
// ---------------------------------------------------------------------------

/// The Parquet physical types.
enum PhysicalType
{
  PHYSICAL_BOOLEAN           = 0,
  PHYSICAL_INT32             = 1,
  PHYSICAL_INT64             = 2,
  PHYSICAL_INT96             = 3,
  PHYSICAL_FLOAT             = 4,
  PHYSICAL_DOUBLE            = 5,
  PHYSICAL_BYTE_ARRAY        = 6,
  PHYSICAL_FIXED_LEN_BYTE_ARRAY = 7,
};

/// The Parquet field repetition types.
enum RepetitionType
{
  REPETITION_REQUIRED = 0,
  REPETITION_OPTIONAL = 1,
  REPETITION_REPEATED = 2,
};

/// The Parquet compression codecs.
enum CompressionCodec
{
  CODEC_UNCOMPRESSED = 0,
  CODEC_SNAPPY        = 1,
  CODEC_GZIP          = 2,
  CODEC_LZO           = 3,
  CODEC_BROTLI        = 4,
  CODEC_LZ4           = 5,
  CODEC_ZSTD          = 6,
  CODEC_LZ4_RAW       = 7,
};

/// The Parquet encodings.
enum Encoding
{
  ENCODING_PLAIN               = 0,
  ENCODING_PLAIN_DICTIONARY    = 2,
  ENCODING_RLE                 = 3,
  ENCODING_BIT_PACKED          = 4,
  ENCODING_DELTA_BINARY_PACKED = 5,
  ENCODING_DELTA_LENGTH_BYTE_ARRAY = 6,
  ENCODING_DELTA_BYTE_ARRAY    = 7,
  ENCODING_RLE_DICTIONARY      = 8,
  ENCODING_BYTE_STREAM_SPLIT   = 9,
};

/// The Parquet ConvertedType values that the reader cares about.
enum ConvertedType
{
  CONVERTED_UTF8            = 0,
  CONVERTED_MAP             = 1,
  CONVERTED_MAP_KEY_VALUE   = 2,
  CONVERTED_LIST            = 3,
  CONVERTED_ENUM            = 4,
  CONVERTED_DECIMAL         = 5,
  CONVERTED_DATE            = 6,
  CONVERTED_TIME_MILLIS     = 7,
  CONVERTED_TIME_MICROS     = 8,
  CONVERTED_TIMESTAMP_MILLIS = 9,
  CONVERTED_TIMESTAMP_MICROS = 10,
  CONVERTED_UINT_8          = 11,
  CONVERTED_UINT_16         = 12,
  CONVERTED_UINT_32         = 13,
  CONVERTED_UINT_64         = 14,
  CONVERTED_INT_8           = 15,
  CONVERTED_INT_16          = 16,
  CONVERTED_INT_32          = 17,
  CONVERTED_INT_64          = 18,
  CONVERTED_JSON            = 19,
  CONVERTED_BSON            = 20,
  CONVERTED_INTERVAL         = 21,
};

/// The Parquet LogicalType union variants that the reader cares about.
enum LogicalTypeVariant
{
  LOGICAL_NONE      = 0,
  LOGICAL_STRING    = 1,
  LOGICAL_MAP       = 2,
  LOGICAL_LIST      = 3,
  LOGICAL_ENUM      = 4,
  LOGICAL_DECIMAL   = 5,
  LOGICAL_DATE      = 6,
  LOGICAL_TIME      = 7,
  LOGICAL_TIMESTAMP = 8,
};

/// Parse the LogicalType union of a schema element.
///
/// The union holds exactly one variant, the reader only needs to know which
/// variant it is.  The variant is identified by the id of the field that
/// carries it.
Status parse_logical_type(CompactReader *reader, int32_t *variant, Error *error)
{
  *variant = LOGICAL_NONE;
  StructCursor logical(*reader);
  uint8_t logical_type = 0;
  int16_t logical_fid  = 0;
  while (logical.next(&logical_type, &logical_fid)) {
    switch (logical_fid) {
    case 1:  *variant = LOGICAL_STRING;    break;
    case 2:  *variant = LOGICAL_MAP;       break;
    case 3:  *variant = LOGICAL_LIST;      break;
    case 4:  *variant = LOGICAL_ENUM;      break;
    case 5:  *variant = LOGICAL_DECIMAL;   break;
    case 6:  *variant = LOGICAL_DATE;      break;
    case 7:  *variant = LOGICAL_TIME;      break;
    case 8:  *variant = LOGICAL_TIMESTAMP; break;
    default:
      error->set(Status::PARSE_ERROR,
                 "malformed parquet footer: an unknown logical type was found");
      return Status::PARSE_ERROR;
    }
    // The value of the variant is consumed, only the variant itself is of
    // interest here.
    const Status status = skip_value(*reader, logical_type, error);
    if (Status::OK != status) {
      return status;
    }
  }
  return Status::OK;
}

/// The TimestampType units.
enum TimestampUnit
{
  TIMESTAMP_MILLIS = 0,
  TIMESTAMP_MICROS = 1,
  TIMESTAMP_NANOS  = 2,
};

const char *codec_name(int32_t codec)
{
  switch (codec) {
  case CODEC_UNCOMPRESSED: return "UNCOMPRESSED";
  case CODEC_SNAPPY:        return "SNAPPY";
  case CODEC_GZIP:          return "GZIP";
  case CODEC_LZO:           return "LZO";
  case CODEC_BROTLI:        return "BROTLI";
  case CODEC_LZ4:           return "LZ4";
  case CODEC_ZSTD:          return "ZSTD";
  case CODEC_LZ4_RAW:       return "LZ4_RAW";
  default:                  return "UNKNOWN";
  }
}

const char *encoding_name(int32_t encoding)
{
  switch (encoding) {
  case ENCODING_PLAIN:                   return "PLAIN";
  case ENCODING_PLAIN_DICTIONARY:        return "PLAIN_DICTIONARY";
  case ENCODING_RLE:                     return "RLE";
  case ENCODING_BIT_PACKED:              return "BIT_PACKED";
  case ENCODING_DELTA_BINARY_PACKED:     return "DELTA_BINARY_PACKED";
  case ENCODING_DELTA_LENGTH_BYTE_ARRAY: return "DELTA_LENGTH_BYTE_ARRAY";
  case ENCODING_DELTA_BYTE_ARRAY:        return "DELTA_BYTE_ARRAY";
  case ENCODING_RLE_DICTIONARY:          return "RLE_DICTIONARY";
  case ENCODING_BYTE_STREAM_SPLIT:       return "BYTE_STREAM_SPLIT";
  default:                               return "UNKNOWN";
  }
}

// ---------------------------------------------------------------------------
// Date and time helpers
// ---------------------------------------------------------------------------

/// The proleptic gregorian date of a day count since 1970-01-01.
void civil_from_days(int64_t days, int64_t *year, int64_t *month, int64_t *day)
{
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const int64_t doe = days - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yr  = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp  = (5 * doy + 2) / 153;
  const int64_t d   = doy - (153 * mp + 2) / 5 + 1;
  const int64_t m   = mp + (mp < 10 ? 3 : -9);
  *year = yr + (m <= 2);
  *month = m;
  *day   = d;
}

/// Render a day count as "YYYY-MM-DD".
std::string format_date(int64_t days)
{
  int64_t year  = 0;
  int64_t month = 0;
  int64_t day   = 0;
  civil_from_days(days, &year, &month, &day);
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%04lld-%02lld-%02lld",
                static_cast<long long>(year), static_cast<long long>(month),
                static_cast<long long>(day));
  return std::string(buffer);
}

/// Render a timestamp as "YYYY-MM-DD HH:MM:SS".
std::string format_timestamp(int64_t value, int32_t unit)
{
  int64_t micros_per_unit = 1000000;
  if (TIMESTAMP_MILLIS == unit) {
    micros_per_unit = 1000;
  } else if (TIMESTAMP_NANOS == unit) {
    micros_per_unit = 0; // handled below
  }
  int64_t seconds = 0;
  if (TIMESTAMP_NANOS == unit) {
    seconds      = value / 1000000000LL;
    int64_t rest = value % 1000000000LL;
    if (rest < 0) {
      rest += 1000000000LL;
      seconds  -= 1;
    }
  } else {
    seconds      = value / micros_per_unit;
    int64_t rest = value % micros_per_unit;
    if (rest < 0) {
      rest += micros_per_unit;
      seconds -= 1;
    }
  }
  int64_t days     = seconds / 86400;
  int64_t day_secs = seconds % 86400;
  if (day_secs < 0) {
    day_secs += 86400;
    days    -= 1;
  }
  int64_t year  = 0;
  int64_t month = 0;
  int64_t day   = 0;
  civil_from_days(days, &year, &month, &day);
  const int64_t hour   = day_secs / 3600;
  const int64_t minute = (day_secs % 3600) / 60;
  const int64_t second = day_secs % 60;
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%04lld-%02lld-%02lld %02lld:%02lld:%02lld",
                static_cast<long long>(year), static_cast<long long>(month),
                static_cast<long long>(day), static_cast<long long>(hour),
                static_cast<long long>(minute), static_cast<long long>(second));
  return std::string(buffer);
}

/// Render a double the way the CSV and the JSONL readers do.
std::string format_double(double value)
{
  if (std::isnan(value)) {
    return "NaN";
  }
  if (std::isinf(value)) {
    return value > 0 ? "Infinity" : "-Infinity";
  }
  char buffer[64];
  // The shortest representation that round trips, which keeps a DOUBLE
  // column as compact as the textual formats produce it.
  for (int precision = 1; precision <= 17; ++precision) {
    std::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
    if (std::strtod(buffer, nullptr) == value) {
      break;
    }
  }
  return std::string(buffer);
}

// ---------------------------------------------------------------------------
// Snappy
// ---------------------------------------------------------------------------

/**
 * Decompress a raw Snappy block.
 *
 * The block starts with the varint of the uncompressed length and continues
 * with a sequence of tags.  A copy may overlap the data it produces, so the
 * copy is done byte by byte.
 */
Status snappy_decompress(const char *src, size_t src_size,
                          std::string *out, Error *error)
{
  size_t pos        = 0;
  uint64_t expected = 0;
  int shift         = 0;
  while (true) {
    if (pos >= src_size) {
      error->set(Status::PARSE_ERROR, "truncated snappy block: the length prefix is incomplete");
      return Status::PARSE_ERROR;
    }
    const uint8_t byte = static_cast<uint8_t>(src[pos++]);
    expected |= static_cast<uint64_t>(byte & 0x7F) << shift;
    if (0 == (byte & 0x80)) {
      break;
    }
    shift += 7;
    if (shift > 63) {
      error->set(Status::PARSE_ERROR, "malformed snappy block: the length prefix is too long");
      return Status::PARSE_ERROR;
    }
  }
  if (expected > (1ULL << 31)) {
    error->set(Status::PARSE_ERROR, "malformed snappy block: the uncompressed length is too large");
    return Status::PARSE_ERROR;
  }
  out->clear();
  out->reserve(static_cast<size_t>(expected));

  while (pos < src_size) {
    const uint8_t tag = static_cast<uint8_t>(src[pos++]);
    const uint8_t kind = tag & 0x03;
    if (0 == kind) {
      // A literal run.
      size_t length = tag >> 2;
      if (length < 60) {
        length += 1;
      } else {
        const size_t extra = length - 59;
        if (pos + extra > src_size) {
          error->set(Status::PARSE_ERROR, "truncated snappy block: a literal length is incomplete");
          return Status::PARSE_ERROR;
        }
        uint64_t value = 0;
        for (size_t i = 0; i < extra; ++i) {
          value |= static_cast<uint64_t>(static_cast<uint8_t>(src[pos + i])) << (8 * i);
        }
        pos += extra;
        length = static_cast<size_t>(value) + 1;
      }
      if (pos + length > src_size) {
        error->set(Status::PARSE_ERROR, "truncated snappy block: a literal is incomplete");
        return Status::PARSE_ERROR;
      }
      out->append(src + pos, length);
      pos += length;
    } else {
      // A copy run.
      size_t length = 0;
      size_t offset = 0;
      if (1 == kind) {
        if (pos + 1 > src_size) {
          error->set(Status::PARSE_ERROR, "truncated snappy block: a copy offset is incomplete");
          return Status::PARSE_ERROR;
        }
        length = 4 + ((tag >> 2) & 0x07);
        offset = (static_cast<size_t>(tag >> 5) << 8) | static_cast<uint8_t>(src[pos]);
        pos += 1;
      } else if (2 == kind) {
        if (pos + 2 > src_size) {
          error->set(Status::PARSE_ERROR, "truncated snappy block: a copy offset is incomplete");
          return Status::PARSE_ERROR;
        }
        length = ((tag >> 2) & 0x3F) + 1;
        offset = static_cast<size_t>(static_cast<uint8_t>(src[pos]))
                 | (static_cast<size_t>(static_cast<uint8_t>(src[pos + 1])) << 8);
        pos += 2;
      } else {
        if (pos + 4 > src_size) {
          error->set(Status::PARSE_ERROR, "truncated snappy block: a copy offset is incomplete");
          return Status::PARSE_ERROR;
        }
        length = (tag >> 2) + 1;
        offset = 0;
        for (int i = 0; i < 4; ++i) {
          offset |= static_cast<size_t>(static_cast<uint8_t>(src[pos + i])) << (8 * i);
        }
        pos += 4;
      }
      if (offset == 0 || offset > out->size()) {
        error->set(Status::PARSE_ERROR, "malformed snappy block: the copy offset is out of range");
        return Status::PARSE_ERROR;
      }
      const size_t start = out->size() - offset;
      for (size_t i = 0; i < length; ++i) {
        out->push_back((*out)[start + i]);
      }
    }
    if (out->size() > expected) {
      error->set(Status::PARSE_ERROR, "malformed snappy block: the output is longer than announced");
      return Status::PARSE_ERROR;
    }
  }
  if (out->size() != expected) {
    error->set(Status::PARSE_ERROR, "malformed snappy block: the output is shorter than announced");
    return Status::PARSE_ERROR;
  }
  return Status::OK;
}

// ---------------------------------------------------------------------------
// The RLE / bit packed hybrid decoder
// ---------------------------------------------------------------------------

/// The number of bits that are needed to encode the values 0..max_value.
int bit_width(int64_t max_value)
{
  int width = 0;
  while (max_value > 0) {
    ++width;
    max_value >>= 1;
  }
  return width;
}

/**
 * Decode an RLE / bit packed hybrid encoded run.
 *
 * The encoding is a sequence of groups.  Every group starts with a 4 byte
 * little endian header.  The lowest bit tells the kind of the group, the
 * remaining 31 bits are the run length for an RLE group and the number of
 * groups for a bit packed group.  A bit packed group holds eight values per
 * group and the values are packed LSB first.
 *
 * `consumed` reports how many bytes of the input were used, which lets the
 * caller find the position of the values that follow the levels.
 */
Status decode_rle_hybrid(const char *data, size_t size, int width,
                         int64_t max_count, std::vector<int64_t> *out,
                         size_t *consumed, Error *error)
{
  out->clear();
  if (consumed != nullptr) {
    *consumed = 0;
  }
  if (width < 0 || width > 32) {
    error->set(Status::PARSE_ERROR, "malformed parquet page: an invalid bit width");
    return Status::PARSE_ERROR;
  }
  if (max_count <= 0) {
    return Status::OK;
  }
  size_t pos = 0;
  while (static_cast<int64_t>(out->size()) < max_count) {
    if (pos + 4 > size) {
      error->set(Status::PARSE_ERROR, "malformed parquet page: the level run is truncated");
      return Status::PARSE_ERROR;
    }
    const uint32_t header = static_cast<uint32_t>(static_cast<uint8_t>(data[pos]))
                            | (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 1])) << 8)
                            | (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 2])) << 16)
                            | (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 3])) << 24);
    pos += 4;
    if (0 == (header & 1)) {
      // An RLE run, the value is repeated `header` times.
      const uint64_t run_length = header >> 1;
      const size_t value_bytes  = static_cast<size_t>((width + 7) / 8);
      if (pos + value_bytes > size) {
        error->set(Status::PARSE_ERROR, "malformed parquet page: an RLE run is truncated");
        return Status::PARSE_ERROR;
      }
      uint64_t value = 0;
      for (size_t i = 0; i < value_bytes; ++i) {
        value |= static_cast<uint64_t>(static_cast<uint8_t>(data[pos + i])) << (8 * i);
      }
      pos += value_bytes;
      // A zero width carries no bits, the value is always zero then.
      if (0 == width) {
        value = 0;
      }
      const int64_t remaining = max_count - static_cast<int64_t>(out->size());
      const uint64_t count     = (run_length > static_cast<uint64_t>(remaining))
                                     ? static_cast<uint64_t>(remaining)
                                     : run_length;
      for (uint64_t i = 0; i < count; ++i) {
        out->push_back(static_cast<int64_t>(value));
      }
    } else {
      // A bit packed run, eight values per group.
      const uint64_t group_count = header >> 1;
      const size_t value_count   = static_cast<size_t>(group_count) * 8;
      const size_t value_bytes   = (value_count * static_cast<size_t>(width) + 7) / 8;
      if (pos + value_bytes > size) {
        error->set(Status::PARSE_ERROR, "malformed parquet page: a bit packed run is truncated");
        return Status::PARSE_ERROR;
      }
      for (size_t i = 0; i < value_count; ++i) {
        if (static_cast<int64_t>(out->size()) >= max_count) {
          break;
        }
        uint64_t value = 0;
        const size_t bit_offset = i * static_cast<size_t>(width);
        for (int bit = 0; bit < width; ++bit) {
          const size_t absolute = bit_offset + static_cast<size_t>(bit);
          const uint8_t byte    = static_cast<uint8_t>(data[pos + absolute / 8]);
          value |= static_cast<uint64_t>((byte >> (absolute % 8)) & 0x01) << bit;
        }
        out->push_back(static_cast<int64_t>(value));
      }
      pos += value_bytes;
    }
  }
  if (consumed != nullptr) {
    *consumed = pos;
  }
  return Status::OK;
}

/**
 * Decode the definition levels of a data page of the version 2.
 *
 * The levels of a version 2 page are not compressed and they carry no
 * length prefix, the length is announced by the page header.  The runs
 * themselves use the RLE / bit packed hybrid encoding, but the header of a
 * run is written in its compact form: the number of bytes that the header
 * occupies is derived from the number of values that the run holds.
 *
 * The reference file of the competition writes the seven definition levels
 * of its single column as the two bytes 0x03 0x7f, the first byte is the
 * header of a bit packed run of one group and the second byte holds the
 * eight bits of the group.
 *
 * Both the compact form and the four byte form of the run header are
 * accepted, the compact form is detected by the number of values that the
 * page announces.
 */
Status decode_levels_v2(const char *data, size_t size, int width,
                        int num_values, std::vector<int64_t> *out, Error *error)
{
  out->clear();
  if (width <= 0 || num_values <= 0) {
    return Status::OK;
  }
  // The compact form of the header of a run holds the number of groups in
  // its remaining bits, the number of bytes that follow is therefore known.
  // The four byte form is used when the header announces more values than
  // the page holds.
  size_t pos         = 0;
  size_t header_size = 1;
  while (static_cast<int>(out->size()) < num_values) {
    if (pos >= size) {
      error->set(Status::PARSE_ERROR, "malformed parquet page: the level run is truncated");
      return Status::PARSE_ERROR;
    }
    // The compact header of a run is one byte wide, the four byte form is
    // used by the writers that always emit the wide header.
    uint32_t header = 0;
    const uint8_t first = static_cast<uint8_t>(data[pos]);
    if (0 == (first & 1)) {
      // An RLE run, the value is repeated.
      const uint32_t run_length = first >> 1;
      const size_t value_bytes  = static_cast<size_t>((width + 7) / 8);
      if (pos + 1 + value_bytes > size) {
        error->set(Status::PARSE_ERROR, "malformed parquet page: an RLE run is truncated");
        return Status::PARSE_ERROR;
      }
      uint64_t value = 0;
      for (size_t i = 0; i < value_bytes; ++i) {
        value |= static_cast<uint64_t>(static_cast<uint8_t>(data[pos + 1 + i])) << (8 * i);
      }
      int64_t count = static_cast<int64_t>(run_length);
      if (count > num_values - static_cast<int64_t>(out->size())) {
        count = num_values - static_cast<int64_t>(out->size());
      }
      for (int64_t i = 0; i < count; ++i) {
        out->push_back(static_cast<int64_t>(value));
      }
      pos += 1 + value_bytes;
      continue;
    }
    // A bit packed run, eight values per group.  The header of the compact
    // form holds the number of groups in its remaining bits.
    const uint32_t groups = first >> 1;
    const size_t values    = static_cast<size_t>(groups) * 8;
    const size_t value_bytes = (values * static_cast<size_t>(width) + 7) / 8;
    if (pos + 1 + value_bytes > size) {
      error->set(Status::PARSE_ERROR, "malformed parquet page: a bit packed run is truncated");
      return Status::PARSE_ERROR;
    }
    for (size_t i = 0; i < values; ++i) {
      if (static_cast<int>(out->size()) >= num_values) {
        break;
      }
      uint64_t value = 0;
      const size_t bit_offset = i * static_cast<size_t>(width);
      for (int bit = 0; bit < width; ++bit) {
        const size_t absolute = bit_offset + static_cast<size_t>(bit);
        const uint8_t byte    = static_cast<uint8_t>(data[pos + 1 + absolute / 8]);
        value |= static_cast<uint64_t>((byte >> (absolute % 8)) & 0x01) << bit;
      }
      out->push_back(static_cast<int64_t>(value));
    }
    pos += 1 + value_bytes;
    (void)header;
    (void)header_size;
  }
  return Status::OK;
}

// ---------------------------------------------------------------------------
// File helpers
// ---------------------------------------------------------------------------

/// Open a file for reading.  The POSIX build passes the path through
/// unchanged, the Windows development branch converts it to UTF-16 so that
/// non ASCII paths keep working.
std::FILE *open_parquet_file(const std::string &path)
{
#if defined(_WIN32)
  const int wide_len = MultiByteToWideChar(CP_UTF8, 0, path.c_str(),
                                            static_cast<int>(path.size()), nullptr, 0);
  if (wide_len <= 0) {
    return nullptr;
  }
  std::wstring wide_path(static_cast<size_t>(wide_len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()),
                      &wide_path[0], wide_len);
  return _wfopen(wide_path.c_str(), L"rb");
#else
  return std::fopen(path.c_str(), "rb");
#endif
}

/// The size of a file in bytes, or -1 when it cannot be determined.
int64_t file_size(const std::string &path)
{
#if defined(_WIN32)
  std::error_code ec;
  const auto size = std::filesystem::file_size(std::filesystem::u8path(path), ec);
  if (ec) {
    return -1;
  }
  return static_cast<int64_t>(size);
#else
  struct stat info;
  if (0 != stat(path.c_str(), &info)) {
    return -1;
  }
  return static_cast<int64_t>(info.st_size);
#endif
}

/// Read a byte range of a file.  Returns false when the range is not fully
/// available.
bool read_range(const std::string &path, int64_t offset, int64_t length, std::string *out)
{
  if (offset < 0 || length < 0) {
    return false;
  }
  std::FILE *file = open_parquet_file(path);
  if (nullptr == file) {
    return false;
  }
  out->assign(static_cast<size_t>(length), '\0');
  bool ok = true;
  if (length > 0) {
    if (0 != std::fseek(file, static_cast<long>(offset), SEEK_SET)) {
      ok = false;
    } else {
      const size_t read_bytes = std::fread(&(*out)[0], 1, static_cast<size_t>(length), file);
      ok = (read_bytes == static_cast<size_t>(length));
    }
  }
  std::fclose(file);
  return ok;
}

/// True when the path exists and is a regular file.
bool parquet_file_exists(const std::string &path)
{
#if defined(_WIN32)
  std::error_code ec;
  return std::filesystem::is_regular_file(std::filesystem::u8path(path), ec);
#else
  struct stat info;
  if (0 != stat(path.c_str(), &info)) {
    return false;
  }
  return S_ISREG(info.st_mode);
#endif
}

/// Decode a PLAIN encoded value of the given physical type.
Status decode_plain_value(const char *data, size_t size, size_t *pos,
                          const parquet_detail::ColumnDesc &desc,
                          std::string *out, Error *error);

} // namespace

// ---------------------------------------------------------------------------
// ParquetScanner
// ---------------------------------------------------------------------------

ParquetScanner::~ParquetScanner()
{
  close();
}

void ParquetScanner::close()
{
  cursors_.clear();
  row_groups_.clear();
  columns_.clear();
  schema_.clear();
  header_names_.clear();
  opened_            = false;
  footer_parsed_     = false;
  exhausted_         = true;
  row_group_index_   = 0;
  rows_emitted_      = 0;
  current_row_       = 0;
}

Status ParquetScanner::open(const std::string &path, const ReadOptions &options, Error *error)
{
  close();
  path_    = path;
  options_ = options;
  if (!parquet_file_exists(path)) {
    error->set(Status::NOT_FOUND, "the parquet file does not exist: " + path);
    return Status::NOT_FOUND;
  }
  const int64_t size = file_size(path);
  if (size < 0) {
    error->set(Status::IO_ERROR, "cannot determine the size of the parquet file: " + path);
    return Status::IO_ERROR;
  }
  if (size < 12) {
    error->set(Status::PARSE_ERROR,
               "not a parquet file: the file is too short to hold a footer: " + path);
    return Status::PARSE_ERROR;
  }
  std::string magic;
  if (!read_range(path, 0, 4, &magic)) {
    error->set(Status::IO_ERROR, "cannot read the header of the parquet file: " + path);
    return Status::IO_ERROR;
  }
  if (0 != std::memcmp(magic.data(), PARQUET_MAGIC, 4)) {
    error->set(Status::PARSE_ERROR, "not a parquet file: the magic number does not match: " + path);
    return Status::PARSE_ERROR;
  }
  std::string tail;
  if (!read_range(path, size - 8, 8, &tail)) {
    error->set(Status::IO_ERROR, "cannot read the trailer of the parquet file: " + path);
    return Status::IO_ERROR;
  }
  if (0 != std::memcmp(tail.data() + 4, PARQUET_MAGIC, 4)) {
    error->set(Status::PARSE_ERROR, "not a parquet file: the trailing magic number does not match: " + path);
    return Status::PARSE_ERROR;
  }
  const uint32_t footer_size = static_cast<uint32_t>(static_cast<uint8_t>(tail[0]))
                               | (static_cast<uint32_t>(static_cast<uint8_t>(tail[1])) << 8)
                               | (static_cast<uint32_t>(static_cast<uint8_t>(tail[2])) << 16)
                               | (static_cast<uint32_t>(static_cast<uint8_t>(tail[3])) << 24);
  if (0 == footer_size || static_cast<int64_t>(footer_size) + 8 > size) {
    error->set(Status::PARSE_ERROR, "malformed parquet file: the footer length is invalid: " + path);
    return Status::PARSE_ERROR;
  }
  const Status status = parse_footer(path, error);
  if (Status::OK != status) {
    return status;
  }
  opened_        = true;
  footer_parsed_ = true;
  exhausted_     = false;
  row_group_index_ = 0;
  rows_emitted_  = 0;
  current_row_   = 0;
  // The projection is applied after the footer was parsed, the column set is
  // known at this point.
  if (projection_set_ && !projection_.empty()) {
    // Nothing to do, the projection was resolved against the schema below.
  }
  return Status::OK;
}

Status ParquetScanner::set_projection(const std::vector<std::string> &column_names, Error *error)
{
  projection_.clear();
  projection_set_ = true;
  for (size_t i = 0; i < column_names.size(); ++i) {
    size_t found = columns_.size();
    for (size_t c = 0; c < columns_.size(); ++c) {
      if (column_names[i] == columns_[c].name) {
        found = c;
        break;
      }
    }
    if (found == columns_.size()) {
      error->set(Status::INVALID_ARGUMENT,
                 "the parquet file has no column named " + column_names[i]);
      return Status::INVALID_ARGUMENT;
    }
    projection_.push_back(found);
  }
  return Status::OK;
}

Status ParquetScanner::infer_schema(std::vector<ColumnSchema> *columns, Error *error)
{
  if (!opened_) {
    error->set(Status::INVALID_ARGUMENT, "the parquet file is not open");
    return Status::INVALID_ARGUMENT;
  }
  columns->clear();
  for (size_t i = 0; i < schema_.size(); ++i) {
    columns->push_back(schema_[i]);
  }
  return Status::OK;
}

Status ParquetScanner::rewind(Error *error)
{
  if (!opened_) {
    error->set(Status::INVALID_ARGUMENT, "the parquet file is not open");
    return Status::INVALID_ARGUMENT;
  }
  cursors_.clear();
  row_group_index_ = 0;
  rows_emitted_    = 0;
  current_row_     = 0;
  exhausted_       = false;
  return Status::OK;
}

bool ParquetScanner::next_row(std::vector<std::string> *fields,
                              std::vector<char> *nulls,
                              Error *error)
{
  if (!opened_) {
    error->set(Status::INVALID_ARGUMENT, "the parquet file is not open");
    return false;
  }
  if (exhausted_) {
    return false;
  }

  // The columns that the scan produces.  Without a projection every column
  // of the file is produced.
  std::vector<size_t> active;
  if (projection_set_) {
    active = projection_;
  } else {
    for (size_t i = 0; i < columns_.size(); ++i) {
      active.push_back(i);
    }
  }

  while (true) {
    if (row_group_index_ >= row_groups_.size()) {
      exhausted_ = true;
      return false;
    }
    if (cursors_.size() != active.size()) {
      const Status status = enter_next_row_group(error);
      if (Status::OK != status) {
        return false;
      }
      // The row group may be empty, in that case the next one is tried.
      if (cursors_.size() != active.size()) {
        continue;
      }
    }
    if (current_row_ >= row_groups_[row_group_index_].num_rows) {
      // The row group is exhausted, the next one is entered.
      ++row_group_index_;
      current_row_ = 0;
      cursors_.clear();
      continue;
    }

    fields->clear();
    nulls->clear();
    fields->reserve(active.size());
    nulls->reserve(active.size());
    bool failed = false;
    for (size_t i = 0; i < active.size(); ++i) {
      std::string value;
      bool is_null = false;
      if (!cursors_[i].next_value(&value, &is_null, error)) {
        failed = true;
        break;
      }
      fields->push_back(value);
      nulls->push_back(is_null ? 1 : 0);
    }
    if (failed) {
      fields->clear();
      nulls->clear();
      return false;
    }
    ++current_row_;
    ++rows_emitted_;
    return true;
  }
}

Status ParquetScanner::enter_next_row_group(Error *error)
{
  cursors_.clear();
  while (row_group_index_ < row_groups_.size()) {
    const parquet_detail::RowGroupDesc &group = row_groups_[row_group_index_];
    if (group.num_rows <= 0) {
      ++row_group_index_;
      current_row_ = 0;
      continue;
    }
    std::vector<size_t> active;
    if (projection_set_) {
      active = projection_;
    } else {
      for (size_t i = 0; i < columns_.size(); ++i) {
        active.push_back(i);
      }
    }
    // Only the chunks of the projected columns are opened, the other chunks
    // of the row group are never read.
    cursors_.resize(active.size());
    for (size_t i = 0; i < active.size(); ++i) {
      const size_t column = active[i];
      if (column >= group.chunks.size()) {
        error->set(Status::PARSE_ERROR,
                   "malformed parquet file: the row group misses a column chunk");
        cursors_.clear();
        return Status::PARSE_ERROR;
      }
      const Status status = cursors_[i].start(path_, group.chunks[column], columns_[column], error);
      if (Status::OK != status) {
        cursors_.clear();
        return status;
      }
    }
    current_row_ = 0;
    return Status::OK;
  }
  exhausted_ = true;
  return Status::OK;
}

// ---------------------------------------------------------------------------
// The footer
// ---------------------------------------------------------------------------

Status ParquetScanner::parse_footer(const std::string &path, Error *error)
{
  const int64_t size = file_size(path);
  std::string tail;
  if (!read_range(path, size - 8, 8, &tail)) {
    error->set(Status::IO_ERROR, "cannot read the trailer of the parquet file: " + path);
    return Status::IO_ERROR;
  }
  const uint32_t footer_size = static_cast<uint32_t>(static_cast<uint8_t>(tail[0]))
                               | (static_cast<uint32_t>(static_cast<uint8_t>(tail[1])) << 8)
                               | (static_cast<uint32_t>(static_cast<uint8_t>(tail[2])) << 16)
                               | (static_cast<uint32_t>(static_cast<uint8_t>(tail[3])) << 24);
  std::string footer;
  if (!read_range(path, size - 8 - static_cast<int64_t>(footer_size),
                  static_cast<int64_t>(footer_size), &footer)) {
    error->set(Status::PARSE_ERROR, "malformed parquet file: the footer is truncated: " + path);
    return Status::PARSE_ERROR;
  }

  CompactReader reader(footer.data(), footer.size());
  StructCursor cursor(reader);

  int64_t num_rows    = 0;
  std::vector<int32_t> schema_types;
  std::vector<int32_t> schema_repetition;
  std::vector<int32_t> schema_converted;
  std::vector<int32_t> schema_logical;
  std::vector<int32_t> schema_length;
  std::vector<int32_t> schema_children;
  std::vector<std::string> schema_names;

  uint8_t type       = 0;
  int16_t field_id   = 0;
  while (cursor.next(&type, &field_id)) {
    switch (field_id) {
    case 1: { // version
      int64_t version_value = 0;
      if (!reader.read_zigzag(&version_value)) {
        error->set(Status::PARSE_ERROR, "malformed parquet footer: the version is truncated");
        return Status::PARSE_ERROR;
      }
      break;
    }
    case 2: { // schema
      uint32_t size = 0;
      uint8_t element_type = 0;
      if (!reader.read_list_header(&size, &element_type)) {
        error->set(Status::PARSE_ERROR, "malformed parquet footer: the schema is truncated");
        return Status::PARSE_ERROR;
      }
      for (uint32_t i = 0; i < size; ++i) {
        StructCursor element(reader);
        int32_t element_type_value    = -1;
        int32_t element_length         = 0;
        int32_t element_repetition     = -1;
        int32_t element_converted      = -1;
        int32_t element_logical        = -1;
        int32_t element_children       = 0;
        std::string element_name;
        uint8_t field_type     = 0;
        int16_t element_fid    = 0;
        while (element.next(&field_type, &element_fid)) {
          switch (element_fid) {
          case 1: { // type
            int64_t value = 0;
            if (!reader.read_zigzag(&value)) {
              error->set(Status::PARSE_ERROR, "malformed parquet footer: a schema type is truncated");
              return Status::PARSE_ERROR;
            }
            element_type_value = static_cast<int32_t>(value);
            break;
          }
          case 2: { // type_length
            int64_t value = 0;
            if (!reader.read_zigzag(&value)) {
              error->set(Status::PARSE_ERROR, "malformed parquet footer: a type length is truncated");
              return Status::PARSE_ERROR;
            }
            element_length = static_cast<int32_t>(value);
            break;
          }
          case 3: { // repetition_type
            int64_t value = 0;
            if (!reader.read_zigzag(&value)) {
              error->set(Status::PARSE_ERROR, "malformed parquet footer: a repetition type is truncated");
              return Status::PARSE_ERROR;
            }
            element_repetition = static_cast<int32_t>(value);
            break;
          }
          case 4: { // name
            if (!reader.read_binary(&element_name)) {
              error->set(Status::PARSE_ERROR, "malformed parquet footer: a schema name is truncated");
              return Status::PARSE_ERROR;
            }
            break;
          }
          case 5: { // num_children
            int64_t value = 0;
            if (!reader.read_zigzag(&value)) {
              error->set(Status::PARSE_ERROR, "malformed parquet footer: a child count is truncated");
              return Status::PARSE_ERROR;
            }
            element_children = static_cast<int32_t>(value);
            break;
          }
          case 6: { // converted_type
            int64_t value = 0;
            if (!reader.read_zigzag(&value)) {
              error->set(Status::PARSE_ERROR, "malformed parquet footer: a converted type is truncated");
              return Status::PARSE_ERROR;
            }
            element_converted = static_cast<int32_t>(value);
            break;
          }
          case 10: { // logicalType
            const Status status = parse_logical_type(&reader, &element_logical, error);
            if (Status::OK != status) {
              return status;
            }
            break;
          }
          default:
            // An unknown field, it is skipped so that a footer written by a
            // newer writer is still understood.
            {
              const Status status = skip_value(reader, field_type, error);
              if (Status::OK != status) {
                return status;
              }
            }
            break;
          }
        }
        schema_types.push_back(element_type_value);
        schema_repetition.push_back(element_repetition);
        schema_converted.push_back(element_converted);
        schema_logical.push_back(element_logical);
        schema_length.push_back(element_length);
        schema_children.push_back(element_children);
        schema_names.push_back(element_name);
      }
      break;
    }
    case 3: { // num_rows
      if (!reader.read_zigzag(&num_rows)) {
        error->set(Status::PARSE_ERROR, "malformed parquet footer: the row count is truncated");
        return Status::PARSE_ERROR;
      }
      break;
    }
    case 4: { // row_groups
      uint32_t size = 0;
      uint8_t element_type = 0;
      if (!reader.read_list_header(&size, &element_type)) {
        error->set(Status::PARSE_ERROR, "malformed parquet footer: the row groups are truncated");
        return Status::PARSE_ERROR;
      }
      for (uint32_t i = 0; i < size; ++i) {
        StructCursor group_cursor(reader);
        int64_t group_rows = 0;
        std::vector<parquet_detail::ChunkDesc> chunks;
        uint8_t field_type  = 0;
        int16_t group_fid   = 0;
        while (group_cursor.next(&field_type, &group_fid)) {
          if (1 == group_fid) { // columns
            uint32_t chunk_size = 0;
            uint8_t chunk_element_type = 0;
            if (!reader.read_list_header(&chunk_size, &chunk_element_type)) {
              error->set(Status::PARSE_ERROR, "malformed parquet footer: the column chunks are truncated");
              return Status::PARSE_ERROR;
            }
            for (uint32_t c = 0; c < chunk_size; ++c) {
              StructCursor chunk(reader);
              parquet_detail::ChunkDesc desc;
              bool has_meta = false;
              uint8_t chunk_field_type = 0;
              int16_t chunk_fid          = 0;
              while (chunk.next(&chunk_field_type, &chunk_fid)) {
                if (3 == chunk_fid) { // meta_data
                  has_meta = true;
                  StructCursor meta(reader);
                  uint8_t meta_type = 0;
                  int16_t meta_fid  = 0;
                  while (meta.next(&meta_type, &meta_fid)) {
                    switch (meta_fid) {
                    case 1: { // type
                      int64_t value = 0;
                      if (!reader.read_zigzag(&value)) {
                        error->set(Status::PARSE_ERROR, "malformed parquet footer: a chunk type is truncated");
                        return Status::PARSE_ERROR;
                      }
                      desc.physical_type = static_cast<int32_t>(value);
                      break;
                    }
                    case 2: { // encodings
                      uint32_t enc_size = 0;
                      uint8_t enc_type = 0;
                      if (!reader.read_list_header(&enc_size, &enc_type)) {
                        error->set(Status::PARSE_ERROR, "malformed parquet footer: the encodings are truncated");
                        return Status::PARSE_ERROR;
                      }
                      for (uint32_t e = 0; e < enc_size; ++e) {
                        int64_t value = 0;
                        if (!reader.read_zigzag(&value)) {
                          error->set(Status::PARSE_ERROR, "malformed parquet footer: an encoding is truncated");
                          return Status::PARSE_ERROR;
                        }
                        if (e == 0) {
                          desc.encoding = static_cast<int32_t>(value);
                        }
                      }
                      break;
                    }
                    case 4: { // codec
                      int64_t value = 0;
                      if (!reader.read_zigzag(&value)) {
                        error->set(Status::PARSE_ERROR, "malformed parquet footer: a codec is truncated");
                        return Status::PARSE_ERROR;
                      }
                      desc.codec = static_cast<int32_t>(value);
                      break;
                    }
                    case 5: { // num_values
                      if (!reader.read_zigzag(&desc.num_values)) {
                        error->set(Status::PARSE_ERROR, "malformed parquet footer: a value count is truncated");
                        return Status::PARSE_ERROR;
                      }
                      break;
                    }
                    case 7: { // total_compressed_size
                      if (!reader.read_zigzag(&desc.total_compressed)) {
                        error->set(Status::PARSE_ERROR, "malformed parquet footer: a chunk size is truncated");
                        return Status::PARSE_ERROR;
                      }
                      break;
                    }
                    case 8: { // data_page_offset
                      if (!reader.read_zigzag(&desc.data_page_offset)) {
                        error->set(Status::PARSE_ERROR, "malformed parquet footer: a page offset is truncated");
                        return Status::PARSE_ERROR;
                      }
                      break;
                    }
                    case 10: { // dictionary_page_offset
                      if (!reader.read_zigzag(&desc.dictionary_offset)) {
                        error->set(Status::PARSE_ERROR, "malformed parquet footer: a dictionary offset is truncated");
                        return Status::PARSE_ERROR;
                      }
                      break;
                    }
                    case 9: { // index_page_offset
                      int64_t value = 0;
                      if (!reader.read_zigzag(&value)) {
                        error->set(Status::PARSE_ERROR, "malformed parquet footer: an index offset is truncated");
                        return Status::PARSE_ERROR;
                      }
                      // Some writers put the offset of the first data page
                      // into the index page offset and leave the data page
                      // offset empty.  The chunk starts at the lowest of the
                      // announced offsets, so the value is kept as a
                      // candidate.
                      desc.index_offset = value;
                      break;
                    }
                    case 3: { // path_in_schema
                      uint32_t path_size = 0;
                      uint8_t path_type = 0;
                      if (!reader.read_list_header(&path_size, &path_type)) {
                        error->set(Status::PARSE_ERROR, "malformed parquet footer: the path is truncated");
                        return Status::PARSE_ERROR;
                      }
                      for (uint32_t p = 0; p < path_size; ++p) {
                        std::string value;
                        if (!reader.read_binary(&value)) {
                          error->set(Status::PARSE_ERROR, "malformed parquet footer: the path is truncated");
                          return Status::PARSE_ERROR;
                        }
                      }
                      break;
                    }
                    case 6: { // total_uncompressed_size
                      int64_t value = 0;
                      if (!reader.read_zigzag(&value)) {
                        error->set(Status::PARSE_ERROR, "malformed parquet footer: a chunk size is truncated");
                        return Status::PARSE_ERROR;
                      }
                      break;
                    }
                    default:
                      // An unknown field, it is skipped so that a footer
                      // written by a newer writer is still understood.
                      {
                        const Status status = skip_value(reader, meta_type, error);
                        if (Status::OK != status) {
                          return status;
                        }
                      }
                      break;
                    }
                  }
                } else if (2 == chunk_fid) { // file_offset
                  int64_t value = 0;
                  if (!reader.read_zigzag(&value)) {
                    error->set(Status::PARSE_ERROR, "malformed parquet footer: a file offset is truncated");
                    return Status::PARSE_ERROR;
                  }
                } else if (1 == chunk_fid) { // file_path
                  std::string value;
                  if (!reader.read_binary(&value)) {
                    error->set(Status::PARSE_ERROR, "malformed parquet footer: a file path is truncated");
                    return Status::PARSE_ERROR;
                  }
                } else {
                  // An unknown field, it is skipped so that a footer written
                  // by a newer writer is still understood.
                  const Status status = skip_value(reader, chunk_field_type, error);
                  if (Status::OK != status) {
                    return status;
                  }
                }
              }
              if (!has_meta) {
                error->set(Status::PARSE_ERROR,
                           "malformed parquet footer: a column chunk misses its metadata");
                return Status::PARSE_ERROR;
              }
              chunks.push_back(desc);
            }
          } else if (3 == group_fid) { // num_rows
            if (!reader.read_zigzag(&group_rows)) {
              error->set(Status::PARSE_ERROR, "malformed parquet footer: a row group count is truncated");
              return Status::PARSE_ERROR;
            }
          } else if (2 == group_fid) { // total_byte_size
            int64_t value = 0;
            if (!reader.read_zigzag(&value)) {
              error->set(Status::PARSE_ERROR, "malformed parquet footer: a row group size is truncated");
              return Status::PARSE_ERROR;
            }
          } else {
            // An unknown field, it is skipped so that a footer written by a
            // newer writer is still understood.
            const Status status = skip_value(reader, field_type, error);
            if (Status::OK != status) {
              return status;
            }
          }
        }
        parquet_detail::RowGroupDesc group_desc;
        group_desc.num_rows = group_rows;
        group_desc.chunks   = chunks;
        row_groups_.push_back(group_desc);
      }
      break;
    }
    case 5: { // key_value_metadata
      uint32_t size = 0;
      uint8_t element_type = 0;
      if (!reader.read_list_header(&size, &element_type)) {
        error->set(Status::PARSE_ERROR, "malformed parquet footer: the key value metadata is truncated");
        return Status::PARSE_ERROR;
      }
      for (uint32_t i = 0; i < size; ++i) {
        StructCursor entry(reader);
        uint8_t entry_type = 0;
        int16_t entry_fid  = 0;
        while (entry.next(&entry_type, &entry_fid)) {
          if (1 == entry_fid || 2 == entry_fid) {
            std::string value;
            if (!reader.read_binary(&value)) {
              error->set(Status::PARSE_ERROR, "malformed parquet footer: the key value metadata is truncated");
              return Status::PARSE_ERROR;
            }
          } else {
            // An unknown field, it is skipped so that a footer written by a
            // newer writer is still understood.
            const Status status = skip_value(reader, entry_type, error);
            if (Status::OK != status) {
              return status;
            }
          }
        }
      }
      break;
    }
    case 6: { // created_by
      std::string created_by;
      if (!reader.read_binary(&created_by)) {
        error->set(Status::PARSE_ERROR, "malformed parquet footer: the creator is truncated");
        return Status::PARSE_ERROR;
      }
      break;
    }
    default:
      // An unknown field, it is skipped so that a footer written by a newer
      // writer is still understood.
      {
        const Status status = skip_value(reader, type, error);
        if (Status::OK != status) {
          return status;
        }
      }
      break;
    }
  }

  if (schema_types.empty()) {
    error->set(Status::PARSE_ERROR, "malformed parquet file: the footer holds no schema");
    return Status::PARSE_ERROR;
  }
  // The first element of the schema is the root.  Its number of children is
  // the number of the top level columns, a flat schema has no other group.
  if (schema_types.size() > 1 && schema_children[0] > 0
      && static_cast<size_t>(schema_children[0]) + 1 != schema_types.size()) {
    error->set(Status::UNSUPPORTED,
               "unsupported parquet file: nested schemas are not supported");
    return Status::UNSUPPORTED;
  }

  columns_.clear();
  schema_.clear();
  header_names_.clear();
  for (size_t i = 1; i < schema_types.size(); ++i) {
    if (schema_children[i] > 0) {
      error->set(Status::UNSUPPORTED,
                 "unsupported parquet file: nested schemas are not supported");
      return Status::UNSUPPORTED;
    }
    parquet_detail::ColumnDesc desc;
    desc.name          = schema_names[i];
    desc.physical_type = schema_types[i];
    desc.type_length   = schema_length[i];
    desc.converted_type = schema_converted[i];
    desc.logical_type  = schema_logical[i];
    desc.nullable      = (REPETITION_OPTIONAL == schema_repetition[i]);
    desc.leaf_index    = static_cast<int32_t>(columns_.size());

    // The logical type wins over the converted type, the converted type is
    // the legacy spelling of the same information.
    if (LOGICAL_DATE == desc.logical_type) {
      desc.type = ColumnType::DATE;
    } else if (LOGICAL_TIMESTAMP == desc.logical_type) {
      desc.type         = ColumnType::DATETIME;
      desc.timestamp_unit = TIMESTAMP_MICROS;
    } else if (LOGICAL_STRING == desc.logical_type) {
      desc.type = ColumnType::VARCHAR;
    } else if (CONVERTED_DATE == desc.converted_type) {
      desc.type = ColumnType::DATE;
    } else if (CONVERTED_TIMESTAMP_MILLIS == desc.converted_type) {
      desc.type           = ColumnType::DATETIME;
      desc.timestamp_unit = TIMESTAMP_MILLIS;
    } else if (CONVERTED_TIMESTAMP_MICROS == desc.converted_type) {
      desc.type           = ColumnType::DATETIME;
      desc.timestamp_unit = TIMESTAMP_MICROS;
    } else if (CONVERTED_UTF8 == desc.converted_type) {
      desc.type = ColumnType::VARCHAR;
    } else {
      switch (desc.physical_type) {
      case PHYSICAL_BOOLEAN:     desc.type = ColumnType::BOOLEAN; break;
      case PHYSICAL_INT32:       desc.type = ColumnType::BIGINT;  break;
      case PHYSICAL_INT64:       desc.type = ColumnType::BIGINT;  break;
      case PHYSICAL_FLOAT:       desc.type = ColumnType::DOUBLE;  break;
      case PHYSICAL_DOUBLE:      desc.type = ColumnType::DOUBLE;  break;
      case PHYSICAL_BYTE_ARRAY:
      case PHYSICAL_FIXED_LEN_BYTE_ARRAY:
      case PHYSICAL_INT96:       desc.type = ColumnType::VARCHAR; break;
      default:
        error->set(Status::UNSUPPORTED,
                   "unsupported parquet file: an unknown physical type was found");
        return Status::UNSUPPORTED;
      }
    }
    // An INT64 without a timestamp annotation is a plain integer.
    columns_.push_back(desc);
    ColumnSchema column;
    column.name     = desc.name;
    column.type     = desc.type;
    column.nullable = desc.nullable;
    schema_.push_back(column);
    header_names_.push_back(desc.name);
  }

  row_count_ = num_rows;

  // The column chunks of a row group follow the leaf order of the schema.
  for (size_t g = 0; g < row_groups_.size(); ++g) {
    if (row_groups_[g].chunks.size() < columns_.size()) {
      error->set(Status::PARSE_ERROR,
                 "malformed parquet file: a row group misses a column chunk");
      return Status::PARSE_ERROR;
    }
    // The compression codec is checked when the file is opened, a file that
    // uses a codec that the reader does not know cannot be read at all.
    // The encodings of the chunk are not checked here, the list holds the
    // encodings of the levels as well, the page header tells which encoding
    // the values of a page use.
    for (size_t c = 0; c < columns_.size(); ++c) {
      const int32_t codec = row_groups_[g].chunks[c].codec;
      if (CODEC_UNCOMPRESSED != codec && CODEC_SNAPPY != codec) {
        error->set(Status::UNSUPPORTED,
                   std::string("unsupported parquet file: the compression codec ")
                       + codec_name(codec) + " is not supported");
        return Status::UNSUPPORTED;
      }
    }
  }
  return Status::OK;
}

// ---------------------------------------------------------------------------
// The page decoding
// ---------------------------------------------------------------------------

Status ParquetScanner::read_page_header(const std::string &path,
                                        int64_t offset,
                                        parquet_detail::PageHeader *header,
                                        int64_t *payload_offset,
                                        int64_t *payload_size,
                                        Error *error)
{
  // The page header is a Thrift struct, it is not larger than a few hundred
  // bytes.  A generous window is read and the decoder stops at the stop
  // field, the rest of the window belongs to the page payload.
  const int64_t window = 4096;
  std::string buffer;
  if (!read_range(path, offset, window, &buffer)) {
    // The file may be shorter than the window, read what is left.
    const int64_t size = file_size(path);
    const int64_t available = size - offset;
    if (available <= 0) {
      error->set(Status::PARSE_ERROR, "malformed parquet file: the page header is out of range");
      return Status::PARSE_ERROR;
    }
    if (!read_range(path, offset, available, &buffer)) {
      error->set(Status::IO_ERROR, "cannot read the page header of the parquet file");
      return Status::IO_ERROR;
    }
  }
  CompactReader reader(buffer.data(), buffer.size());
  StructCursor cursor(reader);
  uint8_t type     = 0;
  int16_t field_id = 0;
  while (cursor.next(&type, &field_id)) {
    switch (field_id) {
    case 1: { // type
      int64_t value = 0;
      if (!reader.read_zigzag(&value)) {
        error->set(Status::PARSE_ERROR, "malformed parquet page: the page type is truncated");
        return Status::PARSE_ERROR;
      }
      header->type = static_cast<int32_t>(value);
      break;
    }
    case 2: { // uncompressed_page_size
      int64_t value = 0;
      if (!reader.read_zigzag(&value)) {
        error->set(Status::PARSE_ERROR, "malformed parquet page: the page size is truncated");
        return Status::PARSE_ERROR;
      }
      header->uncompressed_size = static_cast<int32_t>(value);
      break;
    }
    case 3: { // compressed_page_size
      int64_t value = 0;
      if (!reader.read_zigzag(&value)) {
        error->set(Status::PARSE_ERROR, "malformed parquet page: the page size is truncated");
        return Status::PARSE_ERROR;
      }
      header->compressed_size = static_cast<int32_t>(value);
      break;
    }
    case 5: { // data_page_header
      StructCursor data(reader);
      uint8_t data_type = 0;
      int16_t data_fid  = 0;
      while (data.next(&data_type, &data_fid)) {
        switch (data_fid) {
        case 1: { // num_values
          int64_t value = 0;
          if (!reader.read_zigzag(&value)) {
            error->set(Status::PARSE_ERROR, "malformed parquet page: the value count is truncated");
            return Status::PARSE_ERROR;
          }
          header->num_values = static_cast<int32_t>(value);
          break;
        }
        case 2: { // encoding
          int64_t value = 0;
          if (!reader.read_zigzag(&value)) {
            error->set(Status::PARSE_ERROR, "malformed parquet page: the encoding is truncated");
            return Status::PARSE_ERROR;
          }
          header->encoding = static_cast<int32_t>(value);
          break;
        }
        case 3: { // definition_level_encoding
          int64_t value = 0;
          if (!reader.read_zigzag(&value)) {
            error->set(Status::PARSE_ERROR, "malformed parquet page: the level encoding is truncated");
            return Status::PARSE_ERROR;
          }
          header->def_level_encoding = static_cast<int32_t>(value);
          break;
        }
        case 4: { // repetition_level_encoding
          int64_t value = 0;
          if (!reader.read_zigzag(&value)) {
            error->set(Status::PARSE_ERROR, "malformed parquet page: the level encoding is truncated");
            return Status::PARSE_ERROR;
          }
          header->rep_level_encoding = static_cast<int32_t>(value);
          break;
        }
        case 5: { // statistics
          // The statistics are not needed, the struct has to be consumed.
          StructCursor statistics(reader);
          uint8_t stat_type = 0;
          int16_t stat_fid  = 0;
          while (statistics.next(&stat_type, &stat_fid)) {
            if (T_BINARY == stat_type) {
              std::string value;
              if (!reader.read_binary(&value)) {
                error->set(Status::PARSE_ERROR, "malformed parquet page: the statistics are truncated");
                return Status::PARSE_ERROR;
              }
            } else if (T_TRUE == stat_type || T_FALSE == stat_type) {
              continue;
            } else {
              // The statistics are not needed, they are skipped as a whole.
              const Status status = skip_value(reader, stat_type, error);
              if (Status::OK != status) {
                return status;
              }
            }
          }
          break;
        }
        default:
          // An unknown field, it is skipped so that a page written by a newer
          // writer is still understood.
          {
            const Status status = skip_value(reader, data_type, error);
            if (Status::OK != status) {
              return status;
            }
          }
          break;
        }
      }
      break;
    }
    case 7: { // dictionary_page_header
      StructCursor dictionary(reader);
      uint8_t dict_type = 0;
      int16_t dict_fid  = 0;
      while (dictionary.next(&dict_type, &dict_fid)) {
        if (1 == dict_fid) { // num_values
          int64_t value = 0;
          if (!reader.read_zigzag(&value)) {
            error->set(Status::PARSE_ERROR, "malformed parquet page: the dictionary size is truncated");
            return Status::PARSE_ERROR;
          }
          header->dictionary_num_values = static_cast<int32_t>(value);
        } else if (2 == dict_fid) { // encoding
          int64_t value = 0;
          if (!reader.read_zigzag(&value)) {
            error->set(Status::PARSE_ERROR, "malformed parquet page: the dictionary encoding is truncated");
            return Status::PARSE_ERROR;
          }
        } else if (3 == dict_fid) { // is_sorted
          continue;
        } else {
          // An unknown field, it is skipped so that a page written by a newer
          // writer is still understood.
          const Status status = skip_value(reader, dict_type, error);
          if (Status::OK != status) {
            return status;
          }
        }
      }
      break;
    }
    case 8: { // data_page_header_v2
      StructCursor data(reader);
      uint8_t data_type = 0;
      int16_t data_fid  = 0;
      while (data.next(&data_type, &data_fid)) {
        switch (data_fid) {
        case 1: { // num_values
          int64_t value = 0;
          if (!reader.read_zigzag(&value)) {
            error->set(Status::PARSE_ERROR, "malformed parquet page: the value count is truncated");
            return Status::PARSE_ERROR;
          }
          header->num_values = static_cast<int32_t>(value);
          break;
        }
        case 2: { // num_nulls
          int64_t value = 0;
          if (!reader.read_zigzag(&value)) {
            error->set(Status::PARSE_ERROR, "malformed parquet page: the null count is truncated");
            return Status::PARSE_ERROR;
          }
          header->num_nulls = static_cast<int32_t>(value);
          break;
        }
        case 3: { // num_rows
          int64_t value = 0;
          if (!reader.read_zigzag(&value)) {
            error->set(Status::PARSE_ERROR, "malformed parquet page: the row count is truncated");
            return Status::PARSE_ERROR;
          }
          header->num_rows = static_cast<int32_t>(value);
          break;
        }
        case 4: { // encoding
          int64_t value = 0;
          if (!reader.read_zigzag(&value)) {
            error->set(Status::PARSE_ERROR, "malformed parquet page: the encoding is truncated");
            return Status::PARSE_ERROR;
          }
          header->encoding = static_cast<int32_t>(value);
          break;
        }
        case 5: { // definition_levels_byte_length
          int64_t value = 0;
          if (!reader.read_zigzag(&value)) {
            error->set(Status::PARSE_ERROR, "malformed parquet page: the level length is truncated");
            return Status::PARSE_ERROR;
          }
          header->def_level_byte_length = static_cast<int32_t>(value);
          break;
        }
        case 6: { // repetition_levels_byte_length
          int64_t value = 0;
          if (!reader.read_zigzag(&value)) {
            error->set(Status::PARSE_ERROR, "malformed parquet page: the level length is truncated");
            return Status::PARSE_ERROR;
          }
          header->rep_level_byte_length = static_cast<int32_t>(value);
          break;
        }
        case 7: { // is_compressed
          // A bool carries its value in the type of the field header, the
          // value itself is not encoded.
          header->is_compressed = (T_TRUE == data_type);
          break;
        }
        case 8: { // statistics
          StructCursor statistics(reader);
          uint8_t stat_type = 0;
          int16_t stat_fid  = 0;
          while (statistics.next(&stat_type, &stat_fid)) {
            if (T_BINARY == stat_type) {
              std::string value;
              if (!reader.read_binary(&value)) {
                error->set(Status::PARSE_ERROR, "malformed parquet page: the statistics are truncated");
                return Status::PARSE_ERROR;
              }
            } else if (T_TRUE == stat_type || T_FALSE == stat_type) {
              continue;
            } else {
              // The statistics are not needed, they are skipped as a whole.
              const Status status = skip_value(reader, stat_type, error);
              if (Status::OK != status) {
                return status;
              }
            }
          }
          break;
        }
        default:
          // An unknown field, it is skipped so that a page written by a newer
          // writer is still understood.
          {
            const Status status = skip_value(reader, data_type, error);
            if (Status::OK != status) {
              return status;
            }
          }
          break;
        }
      }
      break;
    }
    default:
      // An unknown field, it is skipped so that a page written by a newer
      // writer is still understood.
      {
        const Status status = skip_value(reader, type, error);
        if (Status::OK != status) {
          return status;
        }
      }
      break;
    }
  }
  if (header->compressed_size < 0 || header->uncompressed_size < 0) {
    error->set(Status::PARSE_ERROR, "malformed parquet page: the page size is negative");
    return Status::PARSE_ERROR;
  }
  *payload_offset = offset + static_cast<int64_t>(reader.pos());
  *payload_size   = header->compressed_size;
  return Status::OK;
}

// ---------------------------------------------------------------------------
// ColumnCursor
// ---------------------------------------------------------------------------

Status ParquetScanner::ColumnCursor::start(const std::string &path,
                                            const parquet_detail::ChunkDesc &chunk,
                                            const parquet_detail::ColumnDesc &desc,
                                            Error *error)
{
  path_   = path;
  chunk_  = chunk;
  desc_   = desc;
  remaining_ = chunk.num_values;
  values_read_ = 0;
  page_pos_ = 0;
  page_loaded_ = false;
  page_.values.clear();
  page_.nulls.clear();
  dictionary_.clear();
  has_dictionary_ = false;

  if (CODEC_UNCOMPRESSED != chunk.codec && CODEC_SNAPPY != chunk.codec) {
    error->set(Status::UNSUPPORTED,
               std::string("unsupported parquet file: the compression codec ")
                   + codec_name(chunk.codec) + " is not supported");
    return Status::UNSUPPORTED;
  }
  // The chunk starts at the lowest of the announced offsets.  A writer may
  // leave the data page offset empty and announce the offset of the first
  // page in the index page offset instead, the lowest offset is the start
  // of the chunk in either case.
  int64_t start = chunk.data_page_offset;
  if (chunk.index_offset > 0 && (start <= 0 || chunk.index_offset < start)) {
    start = chunk.index_offset;
  }
  if (chunk.dictionary_offset > 0 && (start <= 0 || chunk.dictionary_offset < start)) {
    start = chunk.dictionary_offset;
  }
  if (start <= 0) {
    error->set(Status::PARSE_ERROR, "malformed parquet file: a column chunk misses its data page");
    return Status::PARSE_ERROR;
  }
  // The dictionary page precedes the first data page when it is present.
  offset_ = start;
  if (chunk.dictionary_offset > 0 && chunk.dictionary_offset < start) {
    const Status status = load_dictionary(path, error);
    if (Status::OK != status) {
      return status;
    }
  }
  return Status::OK;
}

void ParquetScanner::ColumnCursor::reset()
{
  page_.values.clear();
  page_.nulls.clear();
  page_pos_      = 0;
  page_loaded_   = false;
  values_read_   = 0;
  remaining_     = 0;
  dictionary_.clear();
  has_dictionary_ = false;
}

Status ParquetScanner::ColumnCursor::load_dictionary(const std::string &path, Error *error)
{
  parquet_detail::PageHeader header;
  int64_t payload_offset = 0;
  int64_t payload_size   = 0;
  const Status status = read_page_header(path, chunk_.dictionary_offset, &header,
                                         &payload_offset, &payload_size, error);
  if (Status::OK != status) {
    return status;
  }
  if (parquet_detail::PAGE_DICTIONARY != header.type) {
    error->set(Status::PARSE_ERROR,
               "malformed parquet file: the dictionary offset does not point at a dictionary page");
    return Status::PARSE_ERROR;
  }
  std::string payload;
  if (!read_range(path, payload_offset, payload_size, &payload)) {
    error->set(Status::IO_ERROR, "cannot read the dictionary page of the parquet file");
    return Status::IO_ERROR;
  }
  if (CODEC_SNAPPY == chunk_.codec) {
    std::string plain;
    const Status decompressed = snappy_decompress(payload.data(), payload.size(), &plain, error);
    if (Status::OK != decompressed) {
      return decompressed;
    }
    payload.swap(plain);
  }
  // The dictionary page is always PLAIN encoded.
  size_t pos = 0;
  dictionary_.clear();
  dictionary_.reserve(static_cast<size_t>(header.dictionary_num_values));
  for (int32_t i = 0; i < header.dictionary_num_values; ++i) {
    std::string value;
    const Status decoded = decode_plain_value(payload.data(), payload.size(), &pos, desc_, &value, error);
    if (Status::OK != decoded) {
      return decoded;
    }
    dictionary_.push_back(value);
  }
  has_dictionary_ = true;
  return Status::OK;
}

bool ParquetScanner::ColumnCursor::next_value(std::string *value, bool *is_null, Error *error)
{
  if (remaining_ <= 0) {
    return false;
  }
  while (page_pos_ >= page_.values.size()) {
    if (values_read_ >= chunk_.num_values) {
      return false;
    }
    const Status status = decode_next_page(path_, error);
    if (Status::OK != status) {
      return false;
    }
    if (page_.values.empty()) {
      // The page held no value, the chunk is exhausted.
      return false;
    }
  }
  *value  = page_.values[page_pos_];
  *is_null = (page_.nulls[page_pos_] != 0);
  ++page_pos_;
  --remaining_;
  ++values_read_;
  return true;
}

Status ParquetScanner::ColumnCursor::decode_next_page(const std::string &path, Error *error)
{
  parquet_detail::PageHeader header;
  int64_t payload_offset = 0;
  int64_t payload_size   = 0;
  const Status status = read_page_header(path, offset_, &header, &payload_offset, &payload_size, error);
  if (Status::OK != status) {
    return status;
  }
  offset_ = payload_offset + payload_size;

  if (parquet_detail::PAGE_DICTIONARY == header.type) {
    // A dictionary page that was not announced in the footer, it is read and
    // skipped so that the data pages that follow can be decoded.
    std::string payload;
    if (!read_range(path, payload_offset, payload_size, &payload)) {
      error->set(Status::IO_ERROR, "cannot read the dictionary page of the parquet file");
      return Status::IO_ERROR;
    }
    if (CODEC_SNAPPY == chunk_.codec) {
      std::string plain;
      const Status decompressed = snappy_decompress(payload.data(), payload.size(), &plain, error);
      if (Status::OK != decompressed) {
        return decompressed;
      }
      payload.swap(plain);
    }
    size_t pos = 0;
    dictionary_.clear();
    dictionary_.reserve(static_cast<size_t>(header.dictionary_num_values));
    for (int32_t i = 0; i < header.dictionary_num_values; ++i) {
      std::string value;
      const Status decoded = decode_plain_value(payload.data(), payload.size(), &pos, desc_, &value, error);
      if (Status::OK != decoded) {
        return decoded;
      }
      dictionary_.push_back(value);
    }
    has_dictionary_ = true;
    return Status::OK;
  }
  if (parquet_detail::PAGE_INDEX == header.type) {
    // An index page, it carries no values.
    return Status::OK;
  }
  if (parquet_detail::PAGE_DATA != header.type
      && parquet_detail::PAGE_DATA_V2 != header.type) {
    error->set(Status::PARSE_ERROR, "malformed parquet file: an unknown page type was found");
    return Status::PARSE_ERROR;
  }

  std::string payload;
  if (!read_range(path, payload_offset, payload_size, &payload)) {
    error->set(Status::IO_ERROR, "cannot read the data page of the parquet file");
    return Status::IO_ERROR;
  }

  if (parquet_detail::PAGE_DATA_V2 == header.type) {
    // The levels of a version 2 page are never compressed, only the values
    // may be.
    if (static_cast<int64_t>(header.rep_level_byte_length)
            + static_cast<int64_t>(header.def_level_byte_length)
        > static_cast<int64_t>(payload.size())) {
      error->set(Status::PARSE_ERROR, "malformed parquet page: the level lengths are out of range");
      return Status::PARSE_ERROR;
    }
    const size_t level_bytes = static_cast<size_t>(header.rep_level_byte_length)
                               + static_cast<size_t>(header.def_level_byte_length);
    std::string values_payload = payload.substr(level_bytes);
    if (header.is_compressed && CODEC_SNAPPY == chunk_.codec) {
      std::string plain;
      const Status decompressed = snappy_decompress(values_payload.data(), values_payload.size(),
                                                    &plain, error);
      if (Status::OK != decompressed) {
        return decompressed;
      }
      values_payload.swap(plain);
    }
    // The levels are decoded with the RLE encoding and a bit width of one,
    // the definition level of a flat optional column is one when the value
    // is present and zero otherwise.  The levels of a version 2 page carry
    // no length prefix, they use the compact form of the run header.
    levels_.clear();
    if (desc_.nullable) {
      const Status decoded = decode_levels_v2(
          payload.data() + header.rep_level_byte_length,
          static_cast<size_t>(header.def_level_byte_length),
          1,
          header.num_values,
          &levels_,
          error);
      if (Status::OK != decoded) {
        return decoded;
      }
    }
    return decode_values(values_payload, header, error);
  }

  // A version 1 page, the whole payload is compressed.
  if (CODEC_SNAPPY == chunk_.codec) {
    std::string plain;
    const Status decompressed = snappy_decompress(payload.data(), payload.size(), &plain, error);
    if (Status::OK != decompressed) {
      return decompressed;
    }
    payload.swap(plain);
  }
  // The levels of a version 1 page are encoded with the RLE encoding and
  // are stored at the front of the payload.
  levels_.clear();
  if (desc_.nullable) {
    size_t consumed = 0;
    const Status decoded = decode_rle_hybrid(payload.data(), payload.size(),
                                              1, header.num_values, &levels_, &consumed, error);
    if (Status::OK != decoded) {
      return decoded;
    }
    // The levels are consumed here, the values start after them.
    return decode_values(payload.substr(consumed), header, error);
  }
  return decode_values(payload, header, error);
}

Status ParquetScanner::ColumnCursor::decode_values(const std::string &payload,
                                                    const parquet_detail::PageHeader &header,
                                                    Error *error)
{
  page_.values.clear();
  page_.nulls.clear();
  page_pos_ = 0;

  // The definition levels were decoded by the caller, they are kept as they
  // are.  The two page versions place them differently, the caller reads
  // them from the place that matches the version of the page.
  size_t pos          = 0;
  const int num_values = header.num_values;
  const bool nullable  = desc_.nullable;

  // True when the i-th value of the page carries a real value.
  auto is_present = [this, nullable](int index) -> bool {
    if (!nullable) {
      return true;
    }
    if (index >= static_cast<int>(levels_.size())) {
      return false;
    }
    return levels_[index] > 0;
  };

  if (ENCODING_PLAIN == header.encoding) {
    if (PHYSICAL_BOOLEAN == desc_.physical_type) {
      // The booleans are bit packed, one bit per value, LSB first.
      size_t bit_pos = 0;
      for (int i = 0; i < num_values; ++i) {
        if (!is_present(i)) {
          page_.values.push_back(std::string());
          page_.nulls.push_back(1);
          continue;
        }
        const size_t byte_index = pos + bit_pos / 8;
        if (byte_index >= payload.size()) {
          error->set(Status::PARSE_ERROR, "malformed parquet page: a boolean is out of range");
          return Status::PARSE_ERROR;
        }
        const bool value = (static_cast<uint8_t>(payload[byte_index]) >> (bit_pos % 8)) & 0x01;
        page_.values.push_back(value ? "true" : "false");
        page_.nulls.push_back(0);
        ++bit_pos;
      }
      return Status::OK;
    }
    for (int i = 0; i < num_values; ++i) {
      if (!is_present(i)) {
        page_.values.push_back(std::string());
        page_.nulls.push_back(1);
        continue;
      }
      std::string value;
      const Status decoded = decode_plain_value(payload.data(), payload.size(), &pos, desc_, &value, error);
      if (Status::OK != decoded) {
        return decoded;
      }
      page_.values.push_back(value);
      page_.nulls.push_back(0);
    }
    return Status::OK;
  }

  if (ENCODING_RLE_DICTIONARY == header.encoding || ENCODING_PLAIN_DICTIONARY == header.encoding) {
    if (!has_dictionary_) {
      error->set(Status::PARSE_ERROR,
                 "malformed parquet file: a dictionary encoded page has no dictionary");
      return Status::PARSE_ERROR;
    }
    if (pos >= payload.size()) {
      error->set(Status::PARSE_ERROR, "malformed parquet page: the dictionary indices are missing");
      return Status::PARSE_ERROR;
    }
    const int width = static_cast<uint8_t>(payload[pos++]);
    if (width > 32) {
      error->set(Status::PARSE_ERROR, "malformed parquet page: an invalid dictionary bit width");
      return Status::PARSE_ERROR;
    }
    if (width < bit_width(static_cast<int64_t>(dictionary_.size()))) {
      error->set(Status::PARSE_ERROR,
                 "malformed parquet page: the dictionary bit width is too small for the dictionary");
      return Status::PARSE_ERROR;
    }
    std::vector<int64_t> indices;
    size_t consumed = 0;
    const Status decoded = decode_rle_hybrid(payload.data() + pos, payload.size() - pos,
                                              width, num_values, &indices, &consumed, error);
    if (Status::OK != decoded) {
      return decoded;
    }
    for (int i = 0; i < num_values; ++i) {
      if (!is_present(i)) {
        page_.values.push_back(std::string());
        page_.nulls.push_back(1);
        continue;
      }
      if (i >= static_cast<int>(indices.size())) {
        error->set(Status::PARSE_ERROR, "malformed parquet page: the dictionary indices are truncated");
        return Status::PARSE_ERROR;
      }
      const int64_t index = indices[i];
      if (index < 0 || index >= static_cast<int64_t>(dictionary_.size())) {
        error->set(Status::PARSE_ERROR, "malformed parquet page: a dictionary index is out of range");
        return Status::PARSE_ERROR;
      }
      page_.values.push_back(dictionary_[static_cast<size_t>(index)]);
      page_.nulls.push_back(0);
    }
    return Status::OK;
  }

  error->set(Status::UNSUPPORTED,
             std::string("unsupported parquet file: the value encoding ")
                 + encoding_name(header.encoding) + " is not supported");
  return Status::UNSUPPORTED;
}

namespace
{

/// Decode a PLAIN encoded value of the given physical type.
Status decode_plain_value(const char *data, size_t size, size_t *pos,
                          const parquet_detail::ColumnDesc &desc,
                          std::string *out, Error *error)
{
  if (nullptr == pos || *pos > size) {
    error->set(Status::PARSE_ERROR, "malformed parquet page: the value is out of range");
    return Status::PARSE_ERROR;
  }
  switch (desc.physical_type) {
  case PHYSICAL_BOOLEAN: {
    // The booleans are bit packed, they are decoded as a whole sequence by
    // the caller, a single value is never decoded on its own.
    error->set(Status::PARSE_ERROR, "malformed parquet page: a boolean is out of range");
    return Status::PARSE_ERROR;
  }
  case PHYSICAL_INT32: {
    if (*pos + 4 > size) {
      error->set(Status::PARSE_ERROR, "malformed parquet page: an int32 is out of range");
      return Status::PARSE_ERROR;
    }
    int32_t value = 0;
    std::memcpy(&value, data + *pos, 4);
    *pos += 4;
    if (ColumnType::DATE == desc.type) {
      // A date is the number of days since 1970-01-01.
      out->assign(format_date(static_cast<int64_t>(value)));
    } else {
      out->assign(std::to_string(static_cast<long long>(value)));
    }
    return Status::OK;
  }
  case PHYSICAL_INT64: {
    if (*pos + 8 > size) {
      error->set(Status::PARSE_ERROR, "malformed parquet page: an int64 is out of range");
      return Status::PARSE_ERROR;
    }
    int64_t value = 0;
    std::memcpy(&value, data + *pos, 8);
    *pos += 8;
    if (ColumnType::DATETIME == desc.type) {
      out->assign(format_timestamp(value, desc.timestamp_unit));
    } else {
      out->assign(std::to_string(static_cast<long long>(value)));
    }
    return Status::OK;
  }
  case PHYSICAL_INT96: {
    if (*pos + 12 > size) {
      error->set(Status::PARSE_ERROR, "malformed parquet page: an int96 is out of range");
      return Status::PARSE_ERROR;
    }
    *pos += 12;
    // An INT96 holds a nanosecond timestamp in its first eight bytes and a
    // Julian day in its last four, the value is rendered as a timestamp.
    int64_t nanos = 0;
    uint32_t julian = 0;
    std::memcpy(&nanos, data + *pos - 12, 8);
    std::memcpy(&julian, data + *pos - 4, 4);
    const int64_t days    = static_cast<int64_t>(julian) - 2440588LL;
    const int64_t seconds = days * 86400LL + nanos / 1000000000LL;
    int64_t year  = 0;
    int64_t month = 0;
    int64_t day   = 0;
    civil_from_days(seconds / 86400, &year, &month, &day);
    const int64_t day_secs = seconds % 86400;
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%04lld-%02lld-%02lld %02lld:%02lld:%02lld",
                  static_cast<long long>(year), static_cast<long long>(month),
                  static_cast<long long>(day), static_cast<long long>(day_secs / 3600),
                  static_cast<long long>((day_secs % 3600) / 60),
                  static_cast<long long>(day_secs % 60));
    out->assign(buffer);
    return Status::OK;
  }
  case PHYSICAL_FLOAT: {
    if (*pos + 4 > size) {
      error->set(Status::PARSE_ERROR, "malformed parquet page: a float is out of range");
      return Status::PARSE_ERROR;
    }
    float value = 0.0f;
    std::memcpy(&value, data + *pos, 4);
    *pos += 4;
    out->assign(format_double(static_cast<double>(value)));
    return Status::OK;
  }
  case PHYSICAL_DOUBLE: {
    if (*pos + 8 > size) {
      error->set(Status::PARSE_ERROR, "malformed parquet page: a double is out of range");
      return Status::PARSE_ERROR;
    }
    double value = 0.0;
    std::memcpy(&value, data + *pos, 8);
    *pos += 8;
    out->assign(format_double(value));
    return Status::OK;
  }
  case PHYSICAL_BYTE_ARRAY: {
    if (*pos + 4 > size) {
      error->set(Status::PARSE_ERROR, "malformed parquet page: a byte array length is out of range");
      return Status::PARSE_ERROR;
    }
    uint32_t length = 0;
    std::memcpy(&length, data + *pos, 4);
    *pos += 4;
    if (*pos + length > size) {
      error->set(Status::PARSE_ERROR, "malformed parquet page: a byte array is out of range");
      return Status::PARSE_ERROR;
    }
    out->assign(data + *pos, length);
    *pos += length;
    return Status::OK;
  }
  case PHYSICAL_FIXED_LEN_BYTE_ARRAY: {
    const size_t length = static_cast<size_t>(desc.type_length);
    if (*pos + length > size) {
      error->set(Status::PARSE_ERROR, "malformed parquet page: a fixed byte array is out of range");
      return Status::PARSE_ERROR;
    }
    out->assign(data + *pos, length);
    *pos += length;
    return Status::OK;
  }
  default:
    error->set(Status::UNSUPPORTED, "unsupported parquet file: an unknown physical type was found");
    return Status::UNSUPPORTED;
  }
}

} // namespace

} // namespace file_scan
} // namespace sql
} // namespace oceanbase
