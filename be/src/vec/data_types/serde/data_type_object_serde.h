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

#include <glog/logging.h>
#include <stdint.h>

#include <ostream>

#include "common/status.h"
#include "data_type_serde.h"
#include "util/jsonb_writer.h"

namespace doris {
class PValues;
class JsonbValue;

namespace vectorized {
class IColumn;
class Arena;

class DataTypeObjectSerDe : public DataTypeSerDe {
public:
    Status write_column_to_pb(const IColumn& column, PValues& result, int start,
                              int end) const override {
        LOG(FATAL) << "Not support write object column to pb";
    }
    Status read_column_from_pb(IColumn& column, const PValues& arg) const override {
        LOG(FATAL) << "Not support read from pb to object";
    }
    void write_one_cell_to_jsonb(const IColumn& column, JsonbWriter& result, Arena* mem_pool,
                                 int32_t col_id, int row_num) const override {
        LOG(FATAL) << "Not support write object column to json";
    }

    void read_one_cell_from_jsonb(IColumn& column, const JsonbValue* arg) const override {
        LOG(FATAL) << "Not support write json object to column";
    }

    void write_column_to_arrow(const IColumn& column, const UInt8* null_map,
                               arrow::ArrayBuilder* array_builder, int start,
                               int end) const override {
        LOG(FATAL) << "Not support write object column to arrow";
    }
    void read_column_from_arrow(IColumn& column, const arrow::Array* arrow_array, int start,
                                int end, const cctz::time_zone& ctz) const override {
        LOG(FATAL) << "Not support read object column from arrow";
    }
    Status write_column_to_mysql(const IColumn& column, bool return_object_data_as_binary,
                                 std::vector<MysqlRowBuffer<false>>& result, int row_idx, int start,
                                 int end, bool col_const) const override {
        LOG(FATAL) << "Not support write object column to mysql";
    }
    Status write_column_to_mysql(const IColumn& column, bool return_object_data_as_binary,
                                 std::vector<MysqlRowBuffer<true>>& result, int row_idx, int start,
                                 int end, bool col_const) const override {
        LOG(FATAL) << "Not support write object column to mysql";
    }
};
} // namespace vectorized
} // namespace doris
