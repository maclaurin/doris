// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include <fmt/format.h>
#include <gen_cpp/Types_types.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "common/status.h"
#include "runtime/types.h"
#include "util/runtime_profile.h"
#include "vec/aggregate_functions/aggregate_function.h"
#include "vec/data_types/data_type.h"
#include "vec/exprs/vexpr_fwd.h"

namespace doris {
class RuntimeState;
class TupleDescriptor;

namespace vectorized {
class Block;
} // namespace vectorized

// Table Connector for scan data from ODBC/JDBC
class TableConnector {
public:
    TableConnector(const TupleDescriptor* tuple_desc, const std::string& sql_str);
    virtual ~TableConnector() = default;

    virtual Status open(RuntimeState* state, bool read = false) = 0;
    // exec query for table
    virtual Status query() = 0;

    // use in ODBC/JDBC transaction
    virtual Status begin_trans() = 0;  // should be call after connect and before query
    virtual Status abort_trans() = 0;  // should be call after transaction abort
    virtual Status finish_trans() = 0; // should be call after transaction commit

    virtual Status exec_write_sql(const std::u16string& insert_stmt,
                                  const fmt::memory_buffer& _insert_stmt_buffer) = 0;

    //write data into table vectorized
    Status append(const std::string& table_name, vectorized::Block* block,
                  const vectorized::VExprContextSPtrs& _output_vexpr_ctxs, uint32_t start_send_row,
                  uint32_t* num_rows_sent, TOdbcTableType::type table_type = TOdbcTableType::MYSQL);

    void init_profile(RuntimeProfile*);

    std::u16string utf8_to_u16string(const char* first, const char* last);

    Status convert_column_data(const vectorized::ColumnPtr& column_ptr,
                               const vectorized::DataTypePtr& type_ptr, const TypeDescriptor& type,
                               int row, TOdbcTableType::type table_type);

    virtual Status close() { return Status::OK(); }

protected:
    bool _is_open;
    bool _is_in_transaction;
    const TupleDescriptor* _tuple_desc;
    // only use in query
    std::string _sql_str;
    // only use in write
    fmt::memory_buffer _insert_stmt_buffer;

    // profile use in write
    // tuple convert timer, child timer of _append_row_batch_timer
    RuntimeProfile::Counter* _convert_tuple_timer = nullptr;
    // file write timer, child timer of _append_row_batch_timer
    RuntimeProfile::Counter* _result_send_timer = nullptr;
    // number of sent rows
    RuntimeProfile::Counter* _sent_rows_counter = nullptr;

private:
    // Because Oracle and SAP Hana database do not support
    // insert into tables values (...),(...);
    // Here we do something special for Oracle and SAP Hana.
    Status oracle_type_append(const std::string& table_name, vectorized::Block* block,
                              const vectorized::VExprContextSPtrs& output_vexpr_ctxs,
                              uint32_t start_send_row, uint32_t* num_rows_sent,
                              TOdbcTableType::type table_type);
    Status sap_hana_type_append(const std::string& table_name, vectorized::Block* block,
                                const vectorized::VExprContextSPtrs& output_vexpr_ctxs,
                                uint32_t start_send_row, uint32_t* num_rows_sent,
                                TOdbcTableType::type table_type);
};

} // namespace doris
