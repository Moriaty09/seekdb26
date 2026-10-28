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

namespace oceanbase
{
using namespace common;
namespace sql
{

namespace
{
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
    doc.Parse(line_buf_, static_cast<size_t>(len));
    if (doc.HasParseError() || !doc.IsObject()) {
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
            if (it->value.IsString()) fields.at(idx).assign(it->value.GetString(), it->value.GetStringLength());
            else if (it->value.IsBool()) fields.at(idx) = it->value.GetBool() ? "true" : "false";
            else if (it->value.IsInt64()) fields.at(idx) = std::to_string(it->value.GetInt64());
            else if (it->value.IsUint64()) fields.at(idx) = std::to_string(it->value.GetUint64());
            else if (it->value.IsNumber()) fields.at(idx) = std::to_string(it->value.GetDouble());
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
  bool at_field_start = true;
  bool saw_any = false;
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
    if (quoted) {
      if ('"' == ch) {
        const int next_ch = ::fgetc(file_);
        if ('"' == next_ch) {
          field.push_back('"');
        } else {
          quoted = false;
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
      at_field_start = false;
    } else if (',' == ch) {
      fields.push_back(field);
      field.clear();
      at_field_start = true;
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
      rapidjson::Document doc;
      doc.Parse(line, static_cast<size_t>(len));
      if (doc.HasParseError() || !doc.IsObject()) {
        ret = OB_ERR_INVALID_JSON_TEXT;
      } else {
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
              OZ (def.columns_.push_back(col));
              if (OB_SUCC(ret)) { inferred.push_back(CSV_NULL); idx = inferred.size() - 1; }
            }
            if (OB_SUCC(ret)) {
              CsvInferType type = CSV_STRING;
              if (it->value.IsNull()) type = CSV_NULL;
              else if (it->value.IsBool()) type = CSV_BOOL;
              else if (it->value.IsInt64() || it->value.IsUint64()) type = CSV_INT;
              else if (it->value.IsNumber()) type = CSV_DOUBLE;
              else if (it->value.IsString()) type = classify_csv_value(
                  std::string(it->value.GetString(), it->value.GetStringLength()));
              inferred.at(idx) = widen_csv_type(inferred.at(idx), type);
            }
          }
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
