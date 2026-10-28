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

#ifndef OCEANBASE_BASIC_OB_FUNCTION_TABLE_OP_H_
#define OCEANBASE_BASIC_OB_FUNCTION_TABLE_OP_H_

#include "sql/engine/ob_operator.h"
#include "sql/engine/basic/ob_chunk_datum_store.h"
#include "lib/charset/ob_charset.h"
#include "sql/resolver/dml/ob_dml_stmt.h"
#include "sql/engine/basic/ob_file_table_utils.h"

namespace oceanbase
{
namespace sql
{

class ObExpr;
class ObFunctionTableSpec : public ObOpSpec
{
OB_UNIS_VERSION_V(1);
public:
  ObFunctionTableSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
    : ObOpSpec(alloc, type), value_expr_(nullptr), column_exprs_(alloc), column_names_(alloc), has_correlated_expr_(false),
      file_format_(FILE_TABLE_INVALID), file_path_(), file_header_(true), file_size_(0),
      file_mtime_ns_(0), file_device_(0), file_inode_(0)
  {}
  ObExpr *value_expr_;
  common::ObFixedArray<ObExpr*, common::ObIAllocator> column_exprs_;
  common::ObFixedArray<common::ObString, common::ObIAllocator> column_names_;
  bool has_correlated_expr_;
  ObFileTableFormat file_format_;
  common::ObString file_path_;
  bool file_header_;
  int64_t file_size_;
  int64_t file_mtime_ns_;
  uint64_t file_device_;
  uint64_t file_inode_;
};

class ObFunctionTableOp : public ObOperator
{
public:
  ObFunctionTableOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
    : ObOperator(exec_ctx, spec, input),
    node_idx_(0),
    already_calc_(false),
    row_count_(0),
    col_count_(0),
    value_table_(NULL),
    csv_reader_(),
    jsonl_reader_(),
    parquet_reader_(),
    file_fields_(),
    file_nulls_(),
    glob_paths_(),
    glob_idx_(0)
  {}

  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual int inner_get_next_row() override;
  //virtual int inner_get_next_batch(int64_t max_row_cnt) override;
  virtual int inner_close() override;
  virtual void destroy() override;
private:
  int inner_get_next_row_udf();
  int inner_get_next_row_sys_func();
  int inner_get_next_row_file();
  int read_next_parquet();
  int inner_get_next_row_glob();
  int get_current_result(common::ObObj &result);
  int64_t node_idx_;
  bool already_calc_;
  int64_t row_count_;
  int64_t col_count_;
  common::ObObj value_;
  pl::ObPLCollection *value_table_;
  ObFileTableCsvReader csv_reader_;
  ObFileTableJsonlReader jsonl_reader_;
  ObFileTableParquetReader parquet_reader_;
  std::vector<std::string> file_fields_;
  std::vector<bool> file_nulls_;
  std::vector<std::string> glob_paths_;
  int64_t glob_idx_;
  int (ObFunctionTableOp::*next_row_func_)();
};

} // end namespace sql
} // end namespace oceanbase

#endif /* OCEANBASE_BASIC_OB_FUNCTION_TABLE_OP_H_ */
