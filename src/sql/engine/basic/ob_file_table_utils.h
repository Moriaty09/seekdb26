/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_TABLE_UTILS_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_TABLE_UTILS_H_

#include <stdio.h>
#include <string>
#include <vector>
#include "lib/allocator/ob_allocator.h"
#include "sql/resolver/dml/ob_dml_stmt.h"

namespace oceanbase
{
namespace sql
{

// A small RFC-4180 reader shared by schema inference and execution.  It reads
// one logical record at a time, so quoted newlines do not force whole-file
// materialization.
class ObFileTableCsvReader
{
public:
  ObFileTableCsvReader();
  ~ObFileTableCsvReader();
  int open(const common::ObString &path);
  int close();
  int rewind();
  int next(std::vector<std::string> &fields);
  int64_t line_no() const { return line_no_; }
private:
  FILE *file_;
  std::string path_;
  int64_t line_no_;
  DISALLOW_COPY_AND_ASSIGN(ObFileTableCsvReader);
};

class ObFileTableJsonlReader
{
public:
  ObFileTableJsonlReader();
  ~ObFileTableJsonlReader();
  int open(const common::ObString &path);
  int close();
  int rewind();
  int next(const common::ObIArray<common::ObString> &column_names,
           std::vector<std::string> &fields,
           std::vector<bool> &is_null);
  int64_t line_no() const { return line_no_; }
private:
  FILE *file_;
  std::string path_;
  char *line_buf_;
  size_t line_cap_;
  int64_t line_no_;
  DISALLOW_COPY_AND_ASSIGN(ObFileTableJsonlReader);
};

class ObFileTableUtils
{
public:
  static int canonicalize_and_check(const common::ObString &path,
                                    const common::ObString &secure_file_priv,
                                    common::ObIAllocator &allocator,
                                    common::ObString &canonical_path);
  static int canonicalize_glob_and_check(const common::ObString &pattern,
                                         const common::ObString &secure_file_priv,
                                         common::ObIAllocator &allocator,
                                         common::ObString &canonical_pattern);
  static int infer_csv_schema(common::ObIAllocator &allocator, ObFileTableDef &def);
  static int infer_jsonl_schema(common::ObIAllocator &allocator, ObFileTableDef &def);
  static int validate_unchanged(const ObFileTableDef &def);
private:
  static int make_column_name(common::ObIAllocator &allocator,
                              const std::string &input,
                              int64_t index,
                              const common::ObIArray<ObFileTableColumnDef> &existing,
                              common::ObString &name);
};

} // namespace sql
} // namespace oceanbase
#endif
