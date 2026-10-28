/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/ob_file_table_utils.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "lib/charset/ob_charset.h"
#include "lib/oblog/ob_log.h"
#include "common/timezone/ob_time_convert.h"
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>
#include <strings.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace oceanbase
{
using namespace common;
namespace sql
{

namespace
{
static const size_t FILE_SQL_MAX_RECORD_BYTES = 64L * 1024L * 1024L;
enum CsvInferType { CSV_NULL, CSV_BOOL, CSV_INT, CSV_DOUBLE, CSV_DATE, CSV_DATETIME, CSV_STRING };

CsvInferType classify_csv_value(const std::string &value)
{
  CsvInferType type = CSV_STRING;
  if (value.empty()) {
    type = CSV_NULL;
  } else if (0 == strcasecmp(value.c_str(), "true") ||
             0 == strcasecmp(value.c_str(), "false")) {
    type = CSV_BOOL;
  } else {
    char *end = nullptr;
    errno = 0;
    (void)::strtoll(value.c_str(), &end, 10);
    if (0 == errno && end == value.c_str() + value.size()) {
      type = CSV_INT;
    } else {
      errno = 0;
      end = nullptr;
      (void)::strtod(value.c_str(), &end);
      if (0 == errno && end == value.c_str() + value.size()) {
        type = CSV_DOUBLE;
      } else if (10 == value.size() && '-' == value[4] && '-' == value[7]) {
        int32_t date = 0;
        const ObString str(static_cast<int32_t>(value.size()), value.data());
        type = OB_SUCCESS == ObTimeConverter::str_to_date(str, date) ? CSV_DATE : CSV_STRING;
      } else if (value.size() >= 19 && '-' == value[4] && '-' == value[7] &&
                 (' ' == value[10] || 'T' == value[10])) {
        int64_t datetime = 0;
        ObTimeConvertCtx cvrt_ctx(nullptr, false);
        const ObString str(static_cast<int32_t>(value.size()), value.data());
        type = OB_SUCCESS == ObTimeConverter::str_to_datetime(str, cvrt_ctx, datetime)
                 ? CSV_DATETIME : CSV_STRING;
      }
    }
  }
  return type;
}

CsvInferType widen_csv_type(CsvInferType lhs, CsvInferType rhs)
{
  CsvInferType result = lhs;
  if (CSV_NULL == lhs) {
    result = rhs;
  } else if (CSV_NULL == rhs || lhs == rhs) {
  } else if ((CSV_INT == lhs && CSV_DOUBLE == rhs) ||
             (CSV_DOUBLE == lhs && CSV_INT == rhs)) {
    result = CSV_DOUBLE;
  } else {
    result = CSV_STRING;
  }
  return result;
}

std::string json_scalar_to_string(const rapidjson::Value &value)
{
  std::string result;
  if (value.IsString()) {
    result.assign(value.GetString(), value.GetStringLength());
  } else if (value.IsBool()) {
    result = value.GetBool() ? "true" : "false";
  } else {
    // RapidJSON's writer uses a round-trip-safe representation.  In contrast,
    // std::to_string(double) keeps only six fractional digits and silently
    // changes File SQL values before they reach the SQL conversion layer.
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    if (value.Accept(writer)) {
      result.assign(buffer.GetString(), buffer.GetSize());
    }
  }
  return result;
}

// Parquet metadata uses Thrift's compact protocol.  Keeping this tiny reader
// in-tree avoids pulling a query engine into File SQL; it is only used for the
// footer schema and deliberately rejects malformed or deeply nested input.
class CompactReader
{
public:
  CompactReader(const uint8_t *data, size_t size) : data_(data), size_(size), pos_(0) {}
  int byte(uint8_t &v) { return pos_ < size_ ? (v = data_[pos_++], OB_SUCCESS) : OB_BUF_NOT_ENOUGH; }
  int uvar(uint64_t &v)
  {
    int ret = OB_SUCCESS; v = 0; uint32_t shift = 0; uint8_t b = 0;
    do {
      if (shift >= 64) { ret = OB_INVALID_DATA; break; }
      if (OB_FAIL(byte(b))) break;
      v |= static_cast<uint64_t>(b & 0x7f) << shift; shift += 7;
    } while (b & 0x80);
    return ret;
  }
  int sint(int64_t &v) { uint64_t n = 0; int ret = uvar(n); if (OB_SUCC(ret)) v = static_cast<int64_t>((n >> 1) ^ (~(n & 1) + 1)); return ret; }
  int binary(std::string &v)
  {
    uint64_t n = 0; int ret = uvar(n);
    if (OB_SUCC(ret) && (n > size_ - pos_ || n > FILE_SQL_MAX_RECORD_BYTES)) ret = OB_SIZE_OVERFLOW;
    if (OB_SUCC(ret)) { v.assign(reinterpret_cast<const char *>(data_ + pos_), n); pos_ += n; }
    return ret;
  }
  int field(int16_t &last, int16_t &id, uint8_t &type)
  {
    uint8_t h = 0; int ret = byte(h); type = h & 0x0f; const uint8_t delta = h >> 4;
    if (OB_SUCC(ret) && 0 != type) {
      if (0 != delta) id = last + delta;
      else { int64_t value = 0; if (OB_FAIL(sint(value))) {} else id = static_cast<int16_t>(value); }
      if (OB_SUCC(ret)) last = id;
    }
    return ret;
  }
  int list(uint8_t &type, uint64_t &count)
  {
    uint8_t h = 0; int ret = byte(h); type = h & 0x0f; count = h >> 4;
    if (OB_SUCC(ret) && 15 == count) ret = uvar(count);
    return ret;
  }
  int skip(uint8_t type, int depth = 0)
  {
    int ret = depth > 32 ? OB_SIZE_OVERFLOW : OB_SUCCESS;
    if (OB_FAIL(ret) || 0 == type || 1 == type || 2 == type) {
    } else if (3 == type) { uint8_t v; ret = byte(v); }
    else if (4 == type || 5 == type || 6 == type) { int64_t v; ret = sint(v); }
    else if (7 == type) { if (size_ - pos_ < 8) ret = OB_BUF_NOT_ENOUGH; else pos_ += 8; }
    else if (8 == type) { std::string v; ret = binary(v); }
    else if (9 == type || 10 == type) {
      uint8_t elem = 0; uint64_t count = 0; OZ (list(elem, count));
      for (uint64_t i = 0; OB_SUCC(ret) && i < count; ++i) OZ (skip(elem, depth + 1));
    } else if (11 == type) {
      uint64_t count = 0; OZ (uvar(count));
      if (OB_SUCC(ret) && count > 0) { uint8_t types = 0; OZ (byte(types)); for (uint64_t i = 0; OB_SUCC(ret) && i < count; ++i) { OZ (skip(types >> 4, depth + 1)); OZ (skip(types & 0xf, depth + 1)); } }
    } else if (12 == type) {
      int16_t last = 0, id = 0; uint8_t field_type = 0;
      while (OB_SUCC(ret)) { OZ (field(last, id, field_type)); if (OB_FAIL(ret) || 0 == field_type) break; OZ (skip(field_type, depth + 1)); }
    } else ret = OB_INVALID_DATA;
    return ret;
  }
  size_t position() const { return pos_; }
private:
  const uint8_t *data_;
  size_t size_;
  size_t pos_;
};

struct ParquetSchemaElement
{
  ParquetSchemaElement() : has_type_(false), type_(-1), repetition_(-1), name_(), children_(0) {}
  bool has_type_;
  int64_t type_;
  int64_t repetition_;
  std::string name_;
  int64_t children_;
};

int parse_parquet_schema_element(CompactReader &reader, ParquetSchemaElement &elem)
{
  int ret = OB_SUCCESS; int16_t last = 0, id = 0; uint8_t type = 0;
  while (OB_SUCC(ret)) {
    OZ (reader.field(last, id, type));
    if (OB_FAIL(ret) || 0 == type) break;
    if (1 == id && (4 == type || 5 == type || 6 == type)) { elem.has_type_ = true; OZ (reader.sint(elem.type_)); }
    else if (3 == id && (4 == type || 5 == type || 6 == type)) OZ (reader.sint(elem.repetition_));
    else if (4 == id && 8 == type) OZ (reader.binary(elem.name_));
    else if (5 == id && (4 == type || 5 == type || 6 == type)) OZ (reader.sint(elem.children_));
    else OZ (reader.skip(type));
  }
  return ret;
}

int snappy_decompress(const uint8_t *src, size_t src_len, std::vector<uint8_t> &dst)
{
  int ret = OB_SUCCESS;
  CompactReader length_reader(src, src_len);
  uint64_t expected = 0;
  OZ (length_reader.uvar(expected));
  if (OB_SUCC(ret) && expected > FILE_SQL_MAX_RECORD_BYTES) ret = OB_SIZE_OVERFLOW;
  size_t pos = length_reader.position();
  if (OB_SUCC(ret)) dst.reserve(expected);
  while (OB_SUCC(ret) && pos < src_len && dst.size() < expected) {
    const uint8_t tag = src[pos++];
    const uint8_t kind = tag & 0x03;
    if (0 == kind) {
      uint64_t len = tag >> 2;
      if (len < 60) ++len;
      else {
        const uint8_t bytes = static_cast<uint8_t>(len - 59); len = 0;
        if (bytes > 4 || pos + bytes > src_len) { ret = OB_INVALID_DATA; break; }
        for (uint8_t i = 0; i < bytes; ++i) len |= static_cast<uint64_t>(src[pos++]) << (8 * i);
        ++len;
      }
      if (len > src_len - pos || len > expected - dst.size()) ret = OB_INVALID_DATA;
      else { dst.insert(dst.end(), src + pos, src + pos + len); pos += len; }
    } else {
      uint64_t len = 0, offset = 0;
      if (1 == kind) {
        len = 4 + ((tag >> 2) & 0x7); if (pos >= src_len) { ret = OB_INVALID_DATA; break; }
        offset = ((tag & 0xe0) << 3) | src[pos++];
      } else if (2 == kind) {
        len = 1 + (tag >> 2); if (pos + 2 > src_len) { ret = OB_INVALID_DATA; break; }
        offset = src[pos] | (static_cast<uint64_t>(src[pos + 1]) << 8); pos += 2;
      } else {
        len = 1 + (tag >> 2); if (pos + 4 > src_len) { ret = OB_INVALID_DATA; break; }
        offset = src[pos] | (static_cast<uint64_t>(src[pos + 1]) << 8) |
                 (static_cast<uint64_t>(src[pos + 2]) << 16) | (static_cast<uint64_t>(src[pos + 3]) << 24); pos += 4;
      }
      if (0 == offset || offset > dst.size() || len > expected - dst.size()) ret = OB_INVALID_DATA;
      for (uint64_t i = 0; OB_SUCC(ret) && i < len; ++i) dst.push_back(dst.at(dst.size() - offset));
    }
  }
  if (OB_SUCC(ret) && dst.size() != expected) ret = OB_INVALID_DATA;
  return ret;
}
} // namespace

ObFileTableCsvReader::ObFileTableCsvReader() : file_(nullptr), path_(), line_no_(0) {}
ObFileTableCsvReader::~ObFileTableCsvReader() { close(); }

ObFileTableJsonlReader::ObFileTableJsonlReader()
  : file_(nullptr), path_(), line_buf_(nullptr), line_cap_(0), line_no_(0) {}
ObFileTableJsonlReader::~ObFileTableJsonlReader()
{
  close();
  if (OB_NOT_NULL(line_buf_)) {
    ::free(line_buf_);
    line_buf_ = nullptr;
  }
}

struct ObFileTableParquetReader::Impl
{
  struct Chunk {
    Chunk() : start_(-1), end_(-1), value_count_(0), codec_(-1) {}
    int64_t start_;
    int64_t end_;
    int64_t value_count_;
    int64_t codec_;
  };
  struct Column {
    Column() : physical_type_(-1), chunk_idx_(0), next_offset_(0), chunk_end_(0),
               chunk_remaining_(0), page_value_idx_(0), page_physical_idx_(0) {}
    int64_t physical_type_;
    std::vector<Chunk> chunks_;
    size_t chunk_idx_;
    int64_t next_offset_;
    int64_t chunk_end_;
    int64_t chunk_remaining_;
    int64_t page_value_idx_;
    int64_t page_physical_idx_;
    std::vector<bool> page_nulls_;
    std::vector<std::string> page_values_;
  };
  Impl() : file_(nullptr), remaining_rows_(0), columns_() {}
  FILE *file_;
  int64_t remaining_rows_;
  std::vector<Column> columns_;
};

namespace
{
int parse_parquet_column_metadata(CompactReader &reader,
                                  ObFileTableParquetReader::Impl::Chunk &chunk,
                                  int64_t &physical_type)
{
  int ret = OB_SUCCESS;
  int64_t compressed_size = -1, data_offset = -1, dictionary_offset = -1;
  int16_t last = 0, id = 0; uint8_t type = 0;
  while (OB_SUCC(ret)) {
    OZ (reader.field(last, id, type));
    if (OB_FAIL(ret) || 0 == type) break;
    if ((1 == id || 4 == id || 5 == id || 7 == id || 9 == id || 11 == id) &&
        (4 == type || 5 == type || 6 == type)) {
      int64_t value = 0; OZ (reader.sint(value));
      if (1 == id) physical_type = value;
      else if (4 == id) chunk.codec_ = value;
      else if (5 == id) chunk.value_count_ = value;
      else if (7 == id) compressed_size = value;
      else if (9 == id) data_offset = value;
      else if (11 == id) dictionary_offset = value;
    } else {
      OZ (reader.skip(type));
    }
  }
  if (OB_SUCC(ret)) {
    chunk.start_ = dictionary_offset >= 0 ? std::min(dictionary_offset, data_offset) : data_offset;
    chunk.end_ = chunk.start_ + compressed_size;
    if (physical_type < 0 || chunk.codec_ < 0 || chunk.value_count_ < 0 ||
        chunk.start_ < 4 || compressed_size < 0 || chunk.end_ < chunk.start_) ret = OB_INVALID_DATA;
  }
  return ret;
}

int parse_parquet_column_chunk(CompactReader &reader,
                               ObFileTableParquetReader::Impl::Chunk &chunk,
                               int64_t &physical_type)
{
  int ret = OB_SUCCESS; int16_t last = 0, id = 0; uint8_t type = 0;
  while (OB_SUCC(ret)) {
    OZ (reader.field(last, id, type));
    if (OB_FAIL(ret) || 0 == type) break;
    if (3 == id && 12 == type) OZ (parse_parquet_column_metadata(reader, chunk, physical_type));
    else OZ (reader.skip(type));
  }
  return ret;
}

int parse_parquet_row_group(CompactReader &reader,
                            std::vector<ObFileTableParquetReader::Impl::Chunk> &chunks,
                            std::vector<int64_t> &physical_types,
                            int64_t &row_count)
{
  int ret = OB_SUCCESS; int16_t last = 0, id = 0; uint8_t type = 0;
  while (OB_SUCC(ret)) {
    OZ (reader.field(last, id, type));
    if (OB_FAIL(ret) || 0 == type) break;
    if (1 == id && 9 == type) {
      uint8_t elem_type = 0; uint64_t count = 0; OZ (reader.list(elem_type, count));
      if (OB_SUCC(ret) && (12 != elem_type || count > 10000)) ret = OB_INVALID_DATA;
      chunks.resize(count); physical_types.assign(count, -1);
      for (uint64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
        OZ (parse_parquet_column_chunk(reader, chunks.at(i), physical_types.at(i)));
      }
    } else if (3 == id && (4 == type || 5 == type || 6 == type)) {
      OZ (reader.sint(row_count));
    } else OZ (reader.skip(type));
  }
  return ret;
}
} // namespace

ObFileTableParquetReader::ObFileTableParquetReader() : path_(), impl_(nullptr) {}
ObFileTableParquetReader::~ObFileTableParquetReader() { close(); }

int ObFileTableParquetReader::open(const ObString &path)
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(impl_)) return OB_INIT_TWICE;
  path_.assign(path.ptr(), path.length());
  impl_ = new (std::nothrow) Impl();
  if (OB_ISNULL(impl_)) return OB_ALLOCATE_MEMORY_FAILED;
  struct stat st;
  uint8_t trailer[8] = {0};
  std::vector<uint8_t> footer;
  if (OB_ISNULL(impl_->file_ = ::fopen(path_.c_str(), "rb"))) ret = OB_FILE_NOT_EXIST;
  else if (0 != ::stat(path_.c_str(), &st) || st.st_size < 12) ret = OB_INVALID_DATA;
  else if (0 != ::fseek(impl_->file_, st.st_size - 8, SEEK_SET) || 8 != ::fread(trailer, 1, 8, impl_->file_)) ret = OB_IO_ERROR;
  else if (0 != MEMCMP(trailer + 4, "PAR1", 4)) ret = OB_INVALID_DATA;
  else {
    const uint32_t len = trailer[0] | (trailer[1] << 8) | (trailer[2] << 16) | (trailer[3] << 24);
    const int64_t footer_offset = st.st_size - 8 - len;
    if (len > FILE_SQL_MAX_RECORD_BYTES || footer_offset < 4) ret = OB_SIZE_OVERFLOW;
    else { footer.resize(len); if (0 != ::fseek(impl_->file_, footer_offset, SEEK_SET) || len != ::fread(footer.data(), 1, len, impl_->file_)) ret = OB_IO_ERROR; }
  }
  std::vector<ParquetSchemaElement> schema;
  std::vector<std::vector<Impl::Chunk> > row_groups;
  std::vector<std::vector<int64_t> > row_group_types;
  std::vector<int64_t> row_group_rows;
  if (OB_SUCC(ret)) {
    CompactReader reader(footer.data(), footer.size());
    int16_t last = 0, id = 0; uint8_t type = 0;
    while (OB_SUCC(ret)) {
      OZ (reader.field(last, id, type)); if (OB_FAIL(ret) || 0 == type) break;
      if (2 == id && 9 == type) {
        uint8_t elem_type = 0; uint64_t count = 0; OZ (reader.list(elem_type, count));
        for (uint64_t i = 0; OB_SUCC(ret) && i < count; ++i) { ParquetSchemaElement e; OZ (parse_parquet_schema_element(reader, e)); if (OB_SUCC(ret)) schema.push_back(e); }
      } else if (3 == id && (4 == type || 5 == type || 6 == type)) {
        OZ (reader.sint(impl_->remaining_rows_));
      } else if (4 == id && 9 == type) {
        uint8_t elem_type = 0; uint64_t count = 0; OZ (reader.list(elem_type, count));
        if (OB_SUCC(ret) && (12 != elem_type || count > 100000)) ret = OB_INVALID_DATA;
        row_groups.resize(count); row_group_types.resize(count); row_group_rows.assign(count, -1);
        for (uint64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
          OZ (parse_parquet_row_group(reader, row_groups.at(i), row_group_types.at(i), row_group_rows.at(i)));
        }
      } else OZ (reader.skip(type));
    }
  }
  if (OB_SUCC(ret) && (schema.size() < 2 || row_groups.empty())) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "Parquet file has no flat columns or row groups");
  } else if (OB_SUCC(ret)) {
    const size_t column_count = schema.size() - 1;
    impl_->columns_.resize(column_count);
    int64_t rows_sum = 0;
    for (size_t g = 0; OB_SUCC(ret) && g < row_groups.size(); ++g) {
      if (row_groups.at(g).size() != column_count || row_group_rows.at(g) < 0) ret = OB_INVALID_DATA;
      else rows_sum += row_group_rows.at(g);
      for (size_t c = 0; OB_SUCC(ret) && c < column_count; ++c) {
        if (!schema.at(c + 1).has_type_ || row_group_types.at(g).at(c) != schema.at(c + 1).type_ ||
            row_groups.at(g).at(c).value_count_ != row_group_rows.at(g)) ret = OB_NOT_SUPPORTED;
        else {
          impl_->columns_.at(c).physical_type_ = schema.at(c + 1).type_;
          impl_->columns_.at(c).chunks_.push_back(row_groups.at(g).at(c));
        }
      }
    }
    if (OB_SUCC(ret) && rows_sum != impl_->remaining_rows_) ret = OB_INVALID_DATA;
  }
  if (OB_FAIL(ret)) close();
  return ret;
}

int ObFileTableParquetReader::close()
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(impl_)) {
    if (OB_NOT_NULL(impl_->file_) && 0 != ::fclose(impl_->file_)) ret = OB_IO_ERROR;
    delete impl_;
    impl_ = nullptr;
  }
  return ret;
}

int ObFileTableParquetReader::rewind()
{
  const std::string saved_path = path_;
  int ret = close();
  if (OB_SUCC(ret)) ret = open(ObString(static_cast<int32_t>(saved_path.size()), saved_path.data()));
  return ret;
}

int ObFileTableParquetReader::load_page(int64_t column_idx)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(impl_) || column_idx < 0 || column_idx >= static_cast<int64_t>(impl_->columns_.size())) return OB_INVALID_ARGUMENT;
  Impl::Column &column = impl_->columns_.at(column_idx);
  while (0 == column.chunk_remaining_ && column.chunk_idx_ < column.chunks_.size()) {
    const Impl::Chunk &chunk = column.chunks_.at(column.chunk_idx_++);
    column.next_offset_ = chunk.start_; column.chunk_end_ = chunk.end_; column.chunk_remaining_ = chunk.value_count_;
  }
  if (0 == column.chunk_remaining_ || column.next_offset_ >= column.chunk_end_) ret = OB_ITER_END;
  std::vector<uint8_t> header_buf;
  const int64_t available = column.chunk_end_ - column.next_offset_;
  const size_t probe = static_cast<size_t>(std::min<int64_t>(available, 64 * 1024));
  header_buf.resize(probe);
  if (OB_SUCC(ret) && (0 != ::fseek(impl_->file_, column.next_offset_, SEEK_SET) || probe != ::fread(header_buf.data(), 1, probe, impl_->file_))) ret = OB_IO_ERROR;
  int64_t page_type = -1, uncompressed = -1, compressed = -1, num_values = -1;
  int64_t encoding = -1, def_len = 0, rep_len = 0;
  bool values_compressed = true;
  size_t header_len = 0;
  if (OB_SUCC(ret)) {
    CompactReader reader(header_buf.data(), header_buf.size());
    int16_t last = 0, id = 0; uint8_t type = 0;
    while (OB_SUCC(ret)) {
      OZ (reader.field(last, id, type)); if (OB_FAIL(ret) || 0 == type) break;
      if ((1 == id || 2 == id || 3 == id) && (4 == type || 5 == type || 6 == type)) {
        int64_t value = 0; OZ (reader.sint(value)); if (1 == id) page_type = value; else if (2 == id) uncompressed = value; else compressed = value;
      } else if (8 == id && 12 == type) {
        int16_t nested_last = 0, nested_id = 0; uint8_t nested_type = 0;
        while (OB_SUCC(ret)) {
          OZ (reader.field(nested_last, nested_id, nested_type)); if (OB_FAIL(ret) || 0 == nested_type) break;
          if ((nested_id >= 1 && nested_id <= 6) && (4 == nested_type || 5 == nested_type || 6 == nested_type)) {
            int64_t value = 0; OZ (reader.sint(value));
            if (1 == nested_id) num_values = value; else if (4 == nested_id) encoding = value;
            else if (5 == nested_id) def_len = value; else if (6 == nested_id) rep_len = value;
          } else if (7 == nested_id && (1 == nested_type || 2 == nested_type)) values_compressed = 1 == nested_type;
          else OZ (reader.skip(nested_type));
        }
      } else OZ (reader.skip(type));
    }
    header_len = reader.position();
  }
  if (OB_SUCC(ret) && (3 != page_type || num_values < 0 || num_values > column.chunk_remaining_ || 0 != encoding || compressed < 0 || uncompressed < 0 ||
                       compressed > static_cast<int64_t>(FILE_SQL_MAX_RECORD_BYTES) || rep_len != 0 || def_len < 0 || def_len > compressed)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "unsupported Parquet page encoding");
  }
  std::vector<uint8_t> body;
  if (OB_SUCC(ret)) {
    body.resize(compressed);
    if (column.next_offset_ + static_cast<int64_t>(header_len) + compressed > column.chunk_end_) ret = OB_INVALID_DATA;
    else if (0 != ::fseek(impl_->file_, column.next_offset_ + header_len, SEEK_SET) || compressed != static_cast<int64_t>(::fread(body.data(), 1, compressed, impl_->file_))) ret = OB_IO_ERROR;
    column.next_offset_ += header_len + compressed;
  }
  column.page_nulls_.clear(); column.page_values_.clear(); column.page_value_idx_ = column.page_physical_idx_ = 0;
  if (OB_SUCC(ret)) {
    // Decode the RLE/bit-packed definition levels (bit width one for flat optional columns).
    if (0 == def_len) column.page_nulls_.assign(num_values, false);
    else {
      size_t p = rep_len, end = rep_len + def_len;
      while (OB_SUCC(ret) && p < end && column.page_nulls_.size() < static_cast<size_t>(num_values)) {
        uint64_t run = 0; uint32_t shift = 0; uint8_t b = 0;
        do { if (p >= end || shift >= 64) { ret = OB_INVALID_DATA; break; } b = body[p++]; run |= static_cast<uint64_t>(b & 0x7f) << shift; shift += 7; } while (b & 0x80);
        if (OB_FAIL(ret)) break;
        if (0 == (run & 1)) { const uint64_t count = run >> 1; if (p >= end) { ret = OB_INVALID_DATA; break; } const bool present = 0 != body[p++]; for (uint64_t i = 0; i < count && column.page_nulls_.size() < static_cast<size_t>(num_values); ++i) column.page_nulls_.push_back(!present); }
        else { const uint64_t groups = run >> 1; for (uint64_t g = 0; OB_SUCC(ret) && g < groups; ++g) { if (p >= end) { ret = OB_INVALID_DATA; break; } const uint8_t bits = body[p++]; for (int bit = 0; bit < 8 && column.page_nulls_.size() < static_cast<size_t>(num_values); ++bit) column.page_nulls_.push_back(0 == (bits & (1 << bit))); } }
      }
      if (OB_SUCC(ret) && column.page_nulls_.size() != static_cast<size_t>(num_values)) ret = OB_INVALID_DATA;
    }
  }
  std::vector<uint8_t> values;
  if (OB_SUCC(ret)) {
    const size_t value_off = rep_len + def_len;
    const Impl::Chunk &chunk = column.chunks_.at(column.chunk_idx_ - 1);
    if (values_compressed && 1 == chunk.codec_) OZ (snappy_decompress(body.data() + value_off, body.size() - value_off, values));
    else if (values_compressed && 0 != chunk.codec_) ret = OB_NOT_SUPPORTED;
    else values.assign(body.begin() + value_off, body.end());
  }
  if (OB_SUCC(ret)) {
    size_t p = 0; int64_t non_null = 0; for (bool is_null : column.page_nulls_) if (!is_null) ++non_null;
    for (int64_t i = 0; OB_SUCC(ret) && i < non_null; ++i) {
      std::string value;
      if (0 == column.physical_type_) {
        if (p / 8 >= values.size()) ret = OB_INVALID_DATA; else { value = (values[p / 8] & (1 << (p % 8))) ? "true" : "false"; ++p; }
      } else if (1 == column.physical_type_) {
        if (p + 4 > values.size()) ret = OB_INVALID_DATA; else { int32_t v; MEMCPY(&v, values.data() + p, 4); p += 4; value = std::to_string(v); }
      } else if (2 == column.physical_type_) {
        if (p + 8 > values.size()) ret = OB_INVALID_DATA; else { int64_t v; MEMCPY(&v, values.data() + p, 8); p += 8; value = std::to_string(v); }
      } else if (4 == column.physical_type_) {
        if (p + 4 > values.size()) ret = OB_INVALID_DATA; else { float v; MEMCPY(&v, values.data() + p, 4); p += 4; value = std::to_string(v); }
      } else if (5 == column.physical_type_) {
        if (p + 8 > values.size()) ret = OB_INVALID_DATA; else { double v; MEMCPY(&v, values.data() + p, 8); p += 8; value = std::to_string(v); }
      } else if (6 == column.physical_type_) {
        if (p + 4 > values.size()) ret = OB_INVALID_DATA; else { uint32_t len; MEMCPY(&len, values.data() + p, 4); p += 4; if (len > values.size() - p) ret = OB_INVALID_DATA; else { value.assign(reinterpret_cast<char *>(values.data() + p), len); p += len; } }
      } else ret = OB_NOT_SUPPORTED;
      if (OB_SUCC(ret)) column.page_values_.push_back(value);
    }
  }
  return ret;
}

int ObFileTableParquetReader::next(std::vector<std::string> &fields, std::vector<bool> &is_null)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(impl_)) ret = OB_NOT_INIT;
  else if (impl_->remaining_rows_ <= 0) ret = OB_ITER_END;
  else {
    fields.assign(impl_->columns_.size(), std::string());
    is_null.assign(impl_->columns_.size(), false);
    for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(impl_->columns_.size()); ++i) {
      Impl::Column &column = impl_->columns_.at(i);
      while (OB_SUCC(ret) && column.page_value_idx_ >= static_cast<int64_t>(column.page_nulls_.size())) OZ (load_page(i));
      if (OB_SUCC(ret)) {
        is_null.at(i) = column.page_nulls_.at(column.page_value_idx_++);
        if (!is_null.at(i)) {
          if (column.page_physical_idx_ >= static_cast<int64_t>(column.page_values_.size())) ret = OB_INVALID_DATA;
          else fields.at(i) = column.page_values_.at(column.page_physical_idx_++);
        }
        --column.chunk_remaining_;
      }
    }
    if (OB_SUCC(ret)) --impl_->remaining_rows_;
  }
  return ret;
}

int ObFileTableJsonlReader::open(const ObString &path)
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(file_)) {
    ret = OB_INIT_TWICE;
  } else {
    path_.assign(path.ptr(), path.length());
    if (OB_ISNULL(file_ = ::fopen(path_.c_str(), "rb"))) {
      ret = OB_FILE_NOT_EXIST;
    } else {
      line_no_ = 0;
    }
  }
  return ret;
}

int ObFileTableJsonlReader::close()
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(file_)) {
    if (0 != ::fclose(file_)) ret = OB_IO_ERROR;
    file_ = nullptr;
  }
  return ret;
}

int ObFileTableJsonlReader::rewind()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(file_)) ret = OB_NOT_INIT;
  else if (0 != ::fseek(file_, 0, SEEK_SET)) ret = OB_IO_ERROR;
  else { ::clearerr(file_); line_no_ = 0; }
  return ret;
}

int ObFileTableJsonlReader::next(const ObIArray<ObString> &column_names,
                                 std::vector<std::string> &fields,
                                 std::vector<bool> &is_null)
{
  int ret = OB_SUCCESS;
  fields.assign(column_names.count(), std::string());
  is_null.assign(column_names.count(), true);
  ssize_t len = -1;
  do {
    len = ::getline(&line_buf_, &line_cap_, file_);
    if (len >= 0) ++line_no_;
  } while (len >= 0 && (0 == len || (1 == len && ('\n' == line_buf_[0] || '\r' == line_buf_[0]))));
  if (len < 0) {
    ret = ::ferror(file_) ? OB_IO_ERROR : OB_ITER_END;
  } else {
    while (len > 0 && ('\n' == line_buf_[len - 1] || '\r' == line_buf_[len - 1])) --len;
    rapidjson::Document doc;
    if (static_cast<size_t>(len) > FILE_SQL_MAX_RECORD_BYTES) {
      ret = OB_SIZE_OVERFLOW;
      LOG_USER_ERROR(OB_SIZE_OVERFLOW, "File SQL record exceeds 64 MiB");
    } else {
      doc.Parse(line_buf_, static_cast<size_t>(len));
    }
    if (OB_FAIL(ret)) {
    } else if (doc.HasParseError() || !doc.IsObject()) {
      ret = OB_ERR_INVALID_JSON_TEXT;
    } else {
      for (rapidjson::Value::ConstMemberIterator it = doc.MemberBegin(); OB_SUCC(ret) && it != doc.MemberEnd(); ++it) {
        if (!it->name.IsString() || it->value.IsArray() || it->value.IsObject()) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "nested JSONL values are not supported");
        } else {
          const ObString name(static_cast<int32_t>(it->name.GetStringLength()), it->name.GetString());
          int64_t idx = -1;
          for (int64_t i = 0; i < column_names.count() && idx < 0; ++i) {
            if (ObCharset::case_insensitive_equal(name, column_names.at(i))) idx = i;
          }
          if (idx >= 0 && !it->value.IsNull()) {
            is_null.at(idx) = false;
            if (it->value.IsString() || it->value.IsBool() || it->value.IsNumber()) {
              fields.at(idx) = json_scalar_to_string(it->value);
            }
            else ret = OB_NOT_SUPPORTED;
          }
        }
      }
    }
  }
  return ret;
}

int ObFileTableCsvReader::open(const ObString &path)
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(file_)) {
    ret = OB_INIT_TWICE;
  } else {
    path_.assign(path.ptr(), path.length());
    if (OB_ISNULL(file_ = ::fopen(path_.c_str(), "rb"))) {
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("failed to open file table", K(ret), K(path), K(errno));
    } else {
      line_no_ = 0;
    }
  }
  return ret;
}

int ObFileTableCsvReader::close()
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(file_)) {
    if (0 != ::fclose(file_)) {
      ret = OB_IO_ERROR;
    }
    file_ = nullptr;
  }
  return ret;
}

int ObFileTableCsvReader::rewind()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(file_)) {
    ret = OB_NOT_INIT;
  } else if (0 != ::fseek(file_, 0, SEEK_SET)) {
    ret = OB_IO_ERROR;
  } else {
    ::clearerr(file_);
    line_no_ = 0;
  }
  return ret;
}

int ObFileTableCsvReader::next(std::vector<std::string> &fields)
{
  int ret = OB_SUCCESS;
  fields.clear();
  std::string field;
  bool quoted = false;
  bool just_closed_quote = false;
  bool at_field_start = true;
  bool saw_any = false;
  size_t record_bytes = 0;
  while (OB_SUCC(ret)) {
    const int ch = ::fgetc(file_);
    if (EOF == ch) {
      if (::ferror(file_)) {
        ret = OB_IO_ERROR;
      } else if (!saw_any && field.empty() && fields.empty()) {
        ret = OB_ITER_END;
      } else if (quoted) {
        ret = OB_ERR_UNEXPECTED;
        LOG_USER_ERROR(OB_ERR_UNEXPECTED, "unterminated quoted CSV field");
      } else {
        fields.push_back(field);
      }
      break;
    }
    saw_any = true;
    if (++record_bytes > FILE_SQL_MAX_RECORD_BYTES) {
      ret = OB_SIZE_OVERFLOW;
      LOG_USER_ERROR(OB_SIZE_OVERFLOW, "File SQL record exceeds 64 MiB");
      break;
    }
    if (quoted) {
      if ('"' == ch) {
        const int next_ch = ::fgetc(file_);
        if ('"' == next_ch) {
          if (++record_bytes > FILE_SQL_MAX_RECORD_BYTES) {
            ret = OB_SIZE_OVERFLOW;
            LOG_USER_ERROR(OB_SIZE_OVERFLOW, "File SQL record exceeds 64 MiB");
            break;
          }
          field.push_back('"');
        } else {
          quoted = false;
          just_closed_quote = true;
          if (EOF != next_ch) {
            ::ungetc(next_ch, file_);
          }
        }
      } else {
        field.push_back(static_cast<char>(ch));
        if ('\n' == ch) {
          ++line_no_;
        }
      }
    } else if (at_field_start && '"' == ch) {
      quoted = true;
      just_closed_quote = false;
      at_field_start = false;
    } else if (',' == ch) {
      fields.push_back(field);
      field.clear();
      at_field_start = true;
      just_closed_quote = false;
    } else if ('\n' == ch || '\r' == ch) {
      if ('\r' == ch) {
        const int next_ch = ::fgetc(file_);
        if ('\n' != next_ch && EOF != next_ch) {
          ::ungetc(next_ch, file_);
        }
      }
      ++line_no_;
      fields.push_back(field);
      break;
    } else if (just_closed_quote) {
      ret = OB_INVALID_DATA;
      LOG_USER_ERROR(OB_INVALID_DATA, "unexpected character after closing CSV quote");
    } else {
      field.push_back(static_cast<char>(ch));
      at_field_start = false;
    }
  }
  return ret;
}

int ObFileTableUtils::canonicalize_and_check(const ObString &path,
                                             const ObString &secure_file_priv,
                                             ObIAllocator &allocator,
                                             ObString &canonical_path)
{
  int ret = OB_SUCCESS;
  char input[PATH_MAX + 1] = {0};
  char resolved[PATH_MAX + 1] = {0};
  if (path.empty() || path.length() > PATH_MAX) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    MEMCPY(input, path.ptr(), path.length());
    if (OB_ISNULL(::realpath(input, resolved))) {
      ret = OB_FILE_NOT_EXIST;
    } else {
      const ObString actual(resolved);
      if (OB_FAIL(ObResolverUtils::check_secure_path(secure_file_priv, actual))) {
      } else if (OB_FAIL(ob_write_string(allocator, actual, canonical_path))) {
      }
    }
  }
  return ret;
}

int ObFileTableUtils::canonicalize_glob_and_check(const ObString &pattern,
                                                  const ObString &secure_file_priv,
                                                  ObIAllocator &allocator,
                                                  ObString &canonical_pattern)
{
  int ret = OB_SUCCESS;
  std::string input(pattern.ptr(), pattern.length());
  const size_t slash = input.find_last_of('/');
  const std::string directory = std::string::npos == slash ? "." : input.substr(0, slash);
  const std::string basename = std::string::npos == slash ? input : input.substr(slash + 1);
  ObString canonical_dir;
  const ObString dir_view(static_cast<int32_t>(directory.size()), directory.data());
  if (basename.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(canonicalize_and_check(dir_view, secure_file_priv, allocator, canonical_dir))) {
  } else {
    std::string result(canonical_dir.ptr(), canonical_dir.length());
    if (result.empty() || '/' != result.back()) {
      result.push_back('/');
    }
    result.append(basename);
    const ObString result_view(static_cast<int32_t>(result.size()), result.data());
    OZ (ob_write_string(allocator, result_view, canonical_pattern));
  }
  return ret;
}

int ObFileTableUtils::make_column_name(ObIAllocator &allocator,
                                       const std::string &input,
                                       int64_t index,
                                       const ObIArray<ObFileTableColumnDef> &existing,
                                       ObString &name)
{
  int ret = OB_SUCCESS;
  std::string base = input;
  if (base.empty()) {
    base = "column" + std::to_string(index + 1);
  }
  std::string candidate = base;
  int64_t suffix = 2;
  bool duplicate = true;
  while (duplicate) {
    duplicate = false;
    const ObString candidate_view(static_cast<int32_t>(candidate.size()), candidate.data());
    for (int64_t i = 0; !duplicate && i < existing.count(); ++i) {
      duplicate = ObCharset::case_insensitive_equal(candidate_view, existing.at(i).name_);
    }
    if (duplicate) {
      candidate = base + "__" + std::to_string(suffix++);
    }
  }
  const ObString candidate_view(static_cast<int32_t>(candidate.size()), candidate.data());
  ret = ob_write_string(allocator, candidate_view, name);
  return ret;
}

int ObFileTableUtils::infer_csv_schema(ObIAllocator &allocator, ObFileTableDef &def)
{
  int ret = OB_SUCCESS;
  ObFileTableCsvReader reader;
  std::vector<std::string> fields;
  struct stat st;
  if (OB_FAIL(reader.open(def.path_))) {
  } else if (0 != ::stat(std::string(def.path_.ptr(), def.path_.length()).c_str(), &st)) {
    ret = OB_IO_ERROR;
  } else if (OB_FAIL(reader.next(fields))) {
    if (OB_ITER_END == ret) {
      ret = OB_ERR_UNEXPECTED;
      LOG_USER_ERROR(OB_ERR_UNEXPECTED, "CSV file has no columns");
    }
  } else {
    def.file_size_ = st.st_size;
#if defined(__APPLE__)
    def.file_mtime_ns_ = st.st_mtimespec.tv_sec * 1000000000L + st.st_mtimespec.tv_nsec;
#else
    def.file_mtime_ns_ = st.st_mtim.tv_sec * 1000000000L + st.st_mtim.tv_nsec;
#endif
    def.file_device_ = st.st_dev;
    def.file_inode_ = st.st_ino;
    const int64_t column_count = fields.size();
    std::vector<CsvInferType> inferred(column_count, CSV_NULL);
    for (int64_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
      ObFileTableColumnDef col;
      const std::string header = def.header_ ? fields.at(i) : std::string();
      if (OB_FAIL(make_column_name(allocator, header, i, def.columns_, col.name_))) {
      } else {
        col.meta_.set_varchar();
        col.meta_.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
        col.meta_.set_collation_level(CS_LEVEL_IMPLICIT);
        col.accuracy_.set_length(OB_MAX_VARCHAR_LENGTH);
        if (OB_FAIL(def.columns_.push_back(col))) {
        }
      }
    }
    bool use_current_as_data = !def.header_;
    while (OB_SUCC(ret)) {
      if (!use_current_as_data) {
        ret = reader.next(fields);
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
          break;
        }
      }
      use_current_as_data = false;
      if (OB_FAIL(ret)) {
      } else if (fields.size() != static_cast<size_t>(column_count)) {
        ret = OB_ERR_COLUMN_SIZE;
        LOG_WARN("CSV row has a different column count during inference", K(ret),
                 K(reader.line_no()), K(column_count), "actual", fields.size());
      } else {
        for (int64_t i = 0; i < column_count; ++i) {
          if (fields.at(i).empty()) def.columns_.at(i).nullable_ = true;
          inferred.at(i) = widen_csv_type(inferred.at(i), classify_csv_value(fields.at(i)));
        }
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
      ObFileTableColumnDef &col = def.columns_.at(i);
      switch (inferred.at(i)) {
        case CSV_BOOL:
          col.meta_.set_tinyint();
          col.accuracy_.set_precision(1);
          col.accuracy_.set_scale(0);
          break;
        case CSV_INT:
          col.meta_.set_int();
          col.accuracy_.set_precision(20);
          col.accuracy_.set_scale(0);
          break;
        case CSV_DOUBLE:
          col.meta_.set_double();
          col.accuracy_.set_precision(PRECISION_UNKNOWN_YET);
          col.accuracy_.set_scale(SCALE_UNKNOWN_YET);
          break;
        case CSV_DATE:
          col.meta_.set_date();
          break;
        case CSV_DATETIME:
          col.meta_.set_datetime();
          col.accuracy_.set_scale(0);
          break;
        case CSV_NULL:
        case CSV_STRING:
        default:
          break;
      }
    }
  }
  return ret;
}

int ObFileTableUtils::infer_jsonl_schema(ObIAllocator &allocator, ObFileTableDef &def)
{
  int ret = OB_SUCCESS;
  FILE *file = nullptr;
  char *line = nullptr;
  size_t cap = 0;
  int64_t line_no = 0;
  int64_t data_row_count = 0;
  struct stat st;
  std::vector<CsvInferType> inferred;
  const std::string path(def.path_.ptr(), def.path_.length());
  if (OB_ISNULL(file = ::fopen(path.c_str(), "rb"))) {
    ret = OB_FILE_NOT_EXIST;
  } else if (0 != ::stat(path.c_str(), &st)) {
    ret = OB_IO_ERROR;
  } else {
    def.file_size_ = st.st_size;
#if defined(__APPLE__)
    def.file_mtime_ns_ = st.st_mtimespec.tv_sec * 1000000000L + st.st_mtimespec.tv_nsec;
#else
    def.file_mtime_ns_ = st.st_mtim.tv_sec * 1000000000L + st.st_mtim.tv_nsec;
#endif
    def.file_device_ = st.st_dev;
    def.file_inode_ = st.st_ino;
    ssize_t len = 0;
    while (OB_SUCC(ret) && (len = ::getline(&line, &cap, file)) >= 0) {
      ++line_no;
      while (len > 0 && ('\n' == line[len - 1] || '\r' == line[len - 1])) --len;
      if (0 == len) continue;
      ++data_row_count;
      rapidjson::Document doc;
      if (static_cast<size_t>(len) > FILE_SQL_MAX_RECORD_BYTES) {
        ret = OB_SIZE_OVERFLOW;
        LOG_USER_ERROR(OB_SIZE_OVERFLOW, "File SQL record exceeds 64 MiB");
      } else {
        doc.Parse(line, static_cast<size_t>(len));
      }
      if (OB_FAIL(ret)) {
      } else if (doc.HasParseError() || !doc.IsObject()) {
        ret = OB_ERR_INVALID_JSON_TEXT;
      } else {
        std::vector<bool> present(def.columns_.count(), false);
        for (rapidjson::Value::ConstMemberIterator it = doc.MemberBegin(); OB_SUCC(ret) && it != doc.MemberEnd(); ++it) {
          if (!it->name.IsString() || it->value.IsArray() || it->value.IsObject()) {
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "nested JSONL values are not supported");
          } else {
            const ObString name(static_cast<int32_t>(it->name.GetStringLength()), it->name.GetString());
            int64_t idx = -1;
            for (int64_t i = 0; i < def.columns_.count() && idx < 0; ++i) {
              if (ObCharset::case_insensitive_equal(name, def.columns_.at(i).name_)) idx = i;
            }
            if (idx < 0) {
              ObFileTableColumnDef col;
              std::string key(it->name.GetString(), it->name.GetStringLength());
              OZ (make_column_name(allocator, key, def.columns_.count(), def.columns_, col.name_));
              OX (col.meta_.set_varchar());
              OX (col.meta_.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI));
              OX (col.meta_.set_collation_level(CS_LEVEL_IMPLICIT));
              OX (col.accuracy_.set_length(OB_MAX_VARCHAR_LENGTH));
              OX (col.nullable_ = data_row_count > 1);
              OZ (def.columns_.push_back(col));
              if (OB_SUCC(ret)) {
                inferred.push_back(CSV_NULL);
                present.push_back(true);
                idx = inferred.size() - 1;
              }
            }
            if (OB_SUCC(ret)) {
              present.at(idx) = true;
              CsvInferType type = CSV_STRING;
              if (it->value.IsNull()) {
                type = CSV_NULL;
                def.columns_.at(idx).nullable_ = true;
              }
              else if (it->value.IsBool()) type = CSV_BOOL;
              else if (it->value.IsInt64() ||
                       (it->value.IsUint64() &&
                        it->value.GetUint64() <= static_cast<uint64_t>(INT64_MAX))) type = CSV_INT;
              else if (it->value.IsUint64()) type = CSV_STRING;
              else if (it->value.IsNumber()) type = CSV_DOUBLE;
              else if (it->value.IsString()) type = classify_csv_value(
                  std::string(it->value.GetString(), it->value.GetStringLength()));
              inferred.at(idx) = widen_csv_type(inferred.at(idx), type);
            }
          }
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < def.columns_.count(); ++i) {
          if (!present.at(i)) def.columns_.at(i).nullable_ = true;
        }
      }
    }
    if (OB_SUCC(ret) && ::ferror(file)) ret = OB_IO_ERROR;
    if (OB_SUCC(ret) && def.columns_.empty()) ret = OB_ERR_UNEXPECTED;
    for (int64_t i = 0; OB_SUCC(ret) && i < def.columns_.count(); ++i) {
      ObFileTableColumnDef &col = def.columns_.at(i);
      if (CSV_BOOL == inferred.at(i)) { col.meta_.set_tinyint(); col.accuracy_.set_precision(1); col.accuracy_.set_scale(0); }
      else if (CSV_INT == inferred.at(i)) { col.meta_.set_int(); col.accuracy_.set_precision(20); col.accuracy_.set_scale(0); }
      else if (CSV_DOUBLE == inferred.at(i)) { col.meta_.set_double(); col.accuracy_.set_precision(PRECISION_UNKNOWN_YET); col.accuracy_.set_scale(SCALE_UNKNOWN_YET); }
      else if (CSV_DATE == inferred.at(i)) { col.meta_.set_date(); }
      else if (CSV_DATETIME == inferred.at(i)) { col.meta_.set_datetime(); col.accuracy_.set_scale(0); }
    }
  }
  if (OB_NOT_NULL(line)) ::free(line);
  if (OB_NOT_NULL(file)) ::fclose(file);
  return ret;
}

int ObFileTableUtils::infer_parquet_schema(ObIAllocator &allocator, ObFileTableDef &def)
{
  int ret = OB_SUCCESS;
  FILE *file = nullptr;
  struct stat st;
  const std::string path(def.path_.ptr(), def.path_.length());
  uint8_t trailer[8] = {0};
  std::vector<uint8_t> footer;
  if (OB_ISNULL(file = ::fopen(path.c_str(), "rb"))) {
    ret = OB_FILE_NOT_EXIST;
  } else if (0 != ::stat(path.c_str(), &st) || st.st_size < 12) {
    ret = OB_INVALID_DATA;
  } else if (0 != ::fseek(file, st.st_size - 8, SEEK_SET) ||
             8 != ::fread(trailer, 1, sizeof(trailer), file)) {
    ret = OB_IO_ERROR;
  } else if (0 != MEMCMP(trailer + 4, "PAR1", 4)) {
    ret = OB_INVALID_DATA;
  } else {
    const uint32_t footer_len = static_cast<uint32_t>(trailer[0]) |
                                (static_cast<uint32_t>(trailer[1]) << 8) |
                                (static_cast<uint32_t>(trailer[2]) << 16) |
                                (static_cast<uint32_t>(trailer[3]) << 24);
    if (footer_len > FILE_SQL_MAX_RECORD_BYTES || footer_len > static_cast<uint64_t>(st.st_size - 8)) {
      ret = OB_SIZE_OVERFLOW;
    } else {
      footer.resize(footer_len);
      if (0 != ::fseek(file, st.st_size - 8 - footer_len, SEEK_SET) ||
          footer_len != ::fread(footer.data(), 1, footer_len, file)) {
        ret = OB_IO_ERROR;
      }
    }
  }
  std::vector<ParquetSchemaElement> schema;
  if (OB_SUCC(ret)) {
    CompactReader reader(footer.data(), footer.size());
    int16_t last = 0, id = 0; uint8_t type = 0;
    while (OB_SUCC(ret)) {
      OZ (reader.field(last, id, type));
      if (OB_FAIL(ret) || 0 == type) break;
      if (2 == id && 9 == type) {
        uint8_t elem_type = 0; uint64_t count = 0;
        OZ (reader.list(elem_type, count));
        if (OB_SUCC(ret) && (12 != elem_type || count > 10000)) ret = OB_INVALID_DATA;
        for (uint64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
          ParquetSchemaElement elem;
          OZ (parse_parquet_schema_element(reader, elem));
          if (OB_SUCC(ret)) schema.push_back(elem);
        }
      } else {
        OZ (reader.skip(type));
      }
    }
  }
  if (OB_SUCC(ret) && (schema.size() < 2 || schema.at(0).children_ != static_cast<int64_t>(schema.size() - 1))) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "only flat Parquet schemas are supported");
  }
  for (size_t i = 1; OB_SUCC(ret) && i < schema.size(); ++i) {
    const ParquetSchemaElement &src = schema.at(i);
    ObFileTableColumnDef col;
    if (!src.has_type_ || src.name_.empty() || src.children_ != 0) {
      ret = OB_NOT_SUPPORTED;
    } else {
      OZ (make_column_name(allocator, src.name_, i - 1, def.columns_, col.name_));
      OX (col.nullable_ = 0 != src.repetition_);
      if (OB_FAIL(ret)) {
      } else if (0 == src.type_) {
        col.meta_.set_tinyint(); col.accuracy_.set_precision(1); col.accuracy_.set_scale(0);
      } else if (1 == src.type_ || 2 == src.type_) {
        col.meta_.set_int(); col.accuracy_.set_precision(20); col.accuracy_.set_scale(0);
      } else if (4 == src.type_ || 5 == src.type_) {
        col.meta_.set_double(); col.accuracy_.set_precision(PRECISION_UNKNOWN_YET); col.accuracy_.set_scale(SCALE_UNKNOWN_YET);
      } else if (6 == src.type_ || 7 == src.type_) {
        col.meta_.set_varchar(); col.meta_.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
        col.meta_.set_collation_level(CS_LEVEL_IMPLICIT); col.accuracy_.set_length(OB_MAX_VARCHAR_LENGTH);
      } else if (3 == src.type_) {
        col.meta_.set_datetime(); col.accuracy_.set_scale(6);
      } else {
        ret = OB_NOT_SUPPORTED;
      }
      OZ (def.columns_.push_back(col));
    }
  }
  if (OB_SUCC(ret)) {
    def.file_size_ = st.st_size;
#if defined(__APPLE__)
    def.file_mtime_ns_ = st.st_mtimespec.tv_sec * 1000000000L + st.st_mtimespec.tv_nsec;
#else
    def.file_mtime_ns_ = st.st_mtim.tv_sec * 1000000000L + st.st_mtim.tv_nsec;
#endif
    def.file_device_ = st.st_dev;
    def.file_inode_ = st.st_ino;
  }
  if (OB_NOT_NULL(file)) ::fclose(file);
  return ret;
}

int ObFileTableUtils::validate_unchanged(const ObFileTableDef &def)
{
  int ret = OB_SUCCESS;
  struct stat st;
  const std::string path(def.path_.ptr(), def.path_.length());
  if (0 != ::stat(path.c_str(), &st)) {
    ret = OB_FILE_NOT_EXIST;
  } else {
#if defined(__APPLE__)
    const int64_t mtime_ns = st.st_mtimespec.tv_sec * 1000000000L + st.st_mtimespec.tv_nsec;
#else
    const int64_t mtime_ns = st.st_mtim.tv_sec * 1000000000L + st.st_mtim.tv_nsec;
#endif
    if (def.file_size_ != st.st_size || def.file_mtime_ns_ != mtime_ns ||
        def.file_device_ != static_cast<uint64_t>(st.st_dev) ||
        def.file_inode_ != static_cast<uint64_t>(st.st_ino)) {
      ret = OB_STATE_NOT_MATCH;
      LOG_USER_ERROR(OB_STATE_NOT_MATCH, "file changed after File SQL schema inference");
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
