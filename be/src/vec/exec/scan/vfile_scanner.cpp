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

#include "vec/exec/scan/vfile_scanner.h"

#include <fmt/format.h>
#include <gen_cpp/Exprs_types.h>
#include <gen_cpp/Metrics_types.h>
#include <gen_cpp/PaloInternalService_types.h>
#include <gen_cpp/PlanNodes_types.h>

#include <algorithm>
#include <boost/iterator/iterator_facade.hpp>
#include <iterator>
#include <map>
#include <ostream>
#include <tuple>
#include <utility>

#include "vec/data_types/data_type_factory.hpp"

// IWYU pragma: no_include <opentelemetry/common/threadlocal.h>
#include "common/compiler_util.h" // IWYU pragma: keep
#include "common/config.h"
#include "common/logging.h"
#include "common/object_pool.h"
#include "io/cache/block/block_file_cache_profile.h"
#include "runtime/descriptors.h"
#include "runtime/runtime_state.h"
#include "runtime/types.h"
#include "vec/aggregate_functions/aggregate_function.h"
#include "vec/columns/column.h"
#include "vec/columns/column_nullable.h"
#include "vec/columns/column_vector.h"
#include "vec/columns/columns_number.h"
#include "vec/common/string_ref.h"
#include "vec/core/column_with_type_and_name.h"
#include "vec/core/columns_with_type_and_name.h"
#include "vec/core/field.h"
#include "vec/data_types/data_type.h"
#include "vec/data_types/data_type_nullable.h"
#include "vec/data_types/data_type_number.h"
#include "vec/data_types/data_type_string.h"
#include "vec/exec/format/csv/csv_reader.h"
#include "vec/exec/format/json/new_json_reader.h"
#include "vec/exec/format/orc/vorc_reader.h"
#include "vec/exec/format/parquet/vparquet_reader.h"
#include "vec/exec/format/table/iceberg_reader.h"
#include "vec/exec/scan/new_file_scan_node.h"
#include "vec/exec/scan/vscan_node.h"
#include "vec/exprs/vexpr.h"
#include "vec/exprs/vexpr_context.h"
#include "vec/exprs/vslot_ref.h"
#include "vec/functions/function.h"
#include "vec/functions/simple_function_factory.h"

namespace cctz {
class time_zone;
} // namespace cctz
namespace doris {
namespace vectorized {
class ShardedKVCache;
} // namespace vectorized
} // namespace doris

namespace doris::vectorized {
using namespace ErrorCode;

VFileScanner::VFileScanner(RuntimeState* state, NewFileScanNode* parent, int64_t limit,
                           const TFileScanRange& scan_range, RuntimeProfile* profile,
                           ShardedKVCache* kv_cache)
        : VScanner(state, static_cast<VScanNode*>(parent), limit, profile),
          _params(scan_range.params),
          _ranges(scan_range.ranges),
          _next_range(0),
          _cur_reader(nullptr),
          _cur_reader_eof(false),
          _kv_cache(kv_cache),
          _strict_mode(false) {
    if (scan_range.params.__isset.strict_mode) {
        _strict_mode = scan_range.params.strict_mode;
    }
}

Status VFileScanner::prepare(
        const VExprContextSPtrs& conjuncts,
        std::unordered_map<std::string, ColumnValueRangeType>* colname_to_value_range,
        const std::unordered_map<std::string, int>* colname_to_slot_id) {
    RETURN_IF_ERROR(VScanner::prepare(_state, conjuncts));
    _colname_to_value_range = colname_to_value_range;
    _col_name_to_slot_id = colname_to_slot_id;

    _get_block_timer = ADD_TIMER(_parent->_scanner_profile, "FileScannerGetBlockTime");
    _cast_to_input_block_timer =
            ADD_TIMER(_parent->_scanner_profile, "FileScannerCastInputBlockTime");
    _fill_path_columns_timer =
            ADD_TIMER(_parent->_scanner_profile, "FileScannerFillPathColumnTime");
    _fill_missing_columns_timer =
            ADD_TIMER(_parent->_scanner_profile, "FileScannerFillMissingColumnTime");
    _pre_filter_timer = ADD_TIMER(_parent->_scanner_profile, "FileScannerPreFilterTimer");
    _convert_to_output_block_timer =
            ADD_TIMER(_parent->_scanner_profile, "FileScannerConvertOuputBlockTime");
    _empty_file_counter = ADD_COUNTER(_parent->_scanner_profile, "EmptyFileNum", TUnit::UNIT);

    _file_cache_statistics.reset(new io::FileCacheStatistics());
    _io_ctx.reset(new io::IOContext());
    _io_ctx->file_cache_stats = _file_cache_statistics.get();
    _io_ctx->query_id = &_state->query_id();

    if (_is_load) {
        _src_row_desc.reset(new RowDescriptor(_state->desc_tbl(),
                                              std::vector<TupleId>({_input_tuple_desc->id()}),
                                              std::vector<bool>({false})));
        // prepare pre filters
        if (_params.__isset.pre_filter_exprs_list) {
            RETURN_IF_ERROR(doris::vectorized::VExpr::create_expr_trees(
                    _params.pre_filter_exprs_list, _pre_conjunct_ctxs));
        } else if (_params.__isset.pre_filter_exprs) {
            VExprContextSPtr context;
            RETURN_IF_ERROR(
                    doris::vectorized::VExpr::create_expr_tree(_params.pre_filter_exprs, context));
            _pre_conjunct_ctxs.emplace_back(context);
        }

        for (auto& conjunct : _pre_conjunct_ctxs) {
            RETURN_IF_ERROR(conjunct->prepare(_state, *_src_row_desc));
            RETURN_IF_ERROR(conjunct->open(_state));
        }
    }

    _default_val_row_desc.reset(new RowDescriptor(_state->desc_tbl(),
                                                  std::vector<TupleId>({_real_tuple_desc->id()}),
                                                  std::vector<bool>({false})));

    return Status::OK();
}

Status VFileScanner::_split_conjuncts() {
    for (auto& conjunct : _conjuncts) {
        RETURN_IF_ERROR(_split_conjuncts_expr(conjunct, conjunct->root()));
    }
    return Status::OK();
}
Status VFileScanner::_split_conjuncts_expr(const VExprContextSPtr& context,
                                           const VExprSPtr& conjunct_expr_root) {
    static constexpr auto is_leaf = [](const auto& expr) { return !expr->is_and_expr(); };
    if (conjunct_expr_root) {
        if (is_leaf(conjunct_expr_root)) {
            auto impl = conjunct_expr_root->get_impl();
            // If impl is not null, which means this a conjuncts from runtime filter.
            auto cur_expr = impl ? impl : conjunct_expr_root;
            VExprContextSPtr new_ctx = VExprContext::create_shared(cur_expr);
            context->clone_fn_contexts(new_ctx.get());
            RETURN_IF_ERROR(new_ctx->prepare(_state, *_default_val_row_desc));
            RETURN_IF_ERROR(new_ctx->open(_state));

            std::vector<int> slot_ids;
            _get_slot_ids(cur_expr.get(), &slot_ids);
            if (slot_ids.size() == 0) {
                _not_single_slot_filter_conjuncts.emplace_back(new_ctx);
                return Status::OK();
            }
            bool single_slot = true;
            for (int i = 1; i < slot_ids.size(); i++) {
                if (slot_ids[i] != slot_ids[0]) {
                    single_slot = false;
                    break;
                }
            }
            if (single_slot) {
                SlotId slot_id = slot_ids[0];
                _slot_id_to_filter_conjuncts[slot_id].emplace_back(new_ctx);
            } else {
                _not_single_slot_filter_conjuncts.emplace_back(new_ctx);
            }
        } else {
            RETURN_IF_ERROR(_split_conjuncts_expr(context, conjunct_expr_root->children()[0]));
            RETURN_IF_ERROR(_split_conjuncts_expr(context, conjunct_expr_root->children()[1]));
        }
    }
    return Status::OK();
}

void VFileScanner::_get_slot_ids(VExpr* expr, std::vector<int>* slot_ids) {
    for (auto& child_expr : expr->children()) {
        if (child_expr->is_slot_ref()) {
            VSlotRef* slot_ref = reinterpret_cast<VSlotRef*>(child_expr.get());
            slot_ids->emplace_back(slot_ref->slot_id());
        }
        _get_slot_ids(child_expr.get(), slot_ids);
    }
}

Status VFileScanner::open(RuntimeState* state) {
    RETURN_IF_ERROR(VScanner::open(state));
    RETURN_IF_ERROR(_init_expr_ctxes());

    return Status::OK();
}

// For query:
//                              [exist cols]  [non-exist cols]  [col from path]  input  output
//                              A     B    C  D                 E
// _init_src_block              x     x    x  x                 x                -      x
// get_next_block               x     x    x  -                 -                -      x
// _cast_to_input_block         -     -    -  -                 -                -      -
// _fill_columns_from_path      -     -    -  -                 x                -      x
// _fill_missing_columns        -     -    -  x                 -                -      x
// _convert_to_output_block     -     -    -  -                 -                -      -
//
// For load:
//                              [exist cols]  [non-exist cols]  [col from path]  input  output
//                              A     B    C  D                 E
// _init_src_block              x     x    x  x                 x                x      -
// get_next_block               x     x    x  -                 -                x      -
// _cast_to_input_block         x     x    x  -                 -                x      -
// _fill_columns_from_path      -     -    -  -                 x                x      -
// _fill_missing_columns        -     -    -  x                 -                x      -
// _convert_to_output_block     -     -    -  -                 -                -      x
Status VFileScanner::_get_block_impl(RuntimeState* state, Block* block, bool* eof) {
    do {
        if (_cur_reader == nullptr || _cur_reader_eof) {
            RETURN_IF_ERROR(_get_next_reader());
        }

        if (_scanner_eof) {
            *eof = true;
            return Status::OK();
        }

        // Init src block for load job based on the data file schema (e.g. parquet)
        // For query job, simply set _src_block_ptr to block.
        size_t read_rows = 0;
        RETURN_IF_ERROR(_init_src_block(block));
        {
            SCOPED_TIMER(_get_block_timer);
            // Read next block.
            // Some of column in block may not be filled (column not exist in file)
            RETURN_IF_ERROR(
                    _cur_reader->get_next_block(_src_block_ptr, &read_rows, &_cur_reader_eof));
        }
        // use read_rows instead of _src_block_ptr->rows(), because the first column of _src_block_ptr
        // may not be filled after calling `get_next_block()`, so _src_block_ptr->rows() may return wrong result.
        if (read_rows > 0) {
            // Convert the src block columns type to string in-place.
            RETURN_IF_ERROR(_cast_to_input_block(block));
            // FileReader can fill partition and missing columns itself
            if (!_cur_reader->fill_all_columns()) {
                // Fill rows in src block with partition columns from path. (e.g. Hive partition columns)
                RETURN_IF_ERROR(_fill_columns_from_path(read_rows));
                // Fill columns not exist in file with null or default value
                RETURN_IF_ERROR(_fill_missing_columns(read_rows));
            }
            // Apply _pre_conjunct_ctxs to filter src block.
            RETURN_IF_ERROR(_pre_filter_src_block());
            // Convert src block to output block (dest block), string to dest data type and apply filters.
            RETURN_IF_ERROR(_convert_to_output_block(block));
            break;
        }
    } while (true);

    // Update filtered rows and unselected rows for load, reset counter.
    // {
    //     state->update_num_rows_load_filtered(_counter.num_rows_filtered);
    //     state->update_num_rows_load_unselected(_counter.num_rows_unselected);
    //     _reset_counter();
    // }
    return Status::OK();
}

Status VFileScanner::_init_src_block(Block* block) {
    if (!_is_load) {
        _src_block_ptr = block;
        return Status::OK();
    }

    // if (_src_block_init) {
    //     _src_block.clear_column_data();
    //     _src_block_ptr = &_src_block;
    //     return Status::OK();
    // }

    _src_block.clear();
    size_t idx = 0;
    // slots in _input_tuple_desc contains all slots describe in load statement, eg:
    // -H "columns: k1, k2, tmp1, k3 = tmp1 + 1"
    // _input_tuple_desc will contains: k1, k2, tmp1
    // and some of them are from file, such as k1 and k2, and some of them may not exist in file, such as tmp1
    // _input_tuple_desc also contains columns from path
    for (auto& slot : _input_tuple_desc->slots()) {
        DataTypePtr data_type;
        auto it = _name_to_col_type.find(slot->col_name());
        if (it == _name_to_col_type.end() || _is_dynamic_schema) {
            // not exist in file, using type from _input_tuple_desc
            data_type =
                    DataTypeFactory::instance().create_data_type(slot->type(), slot->is_nullable());
        } else {
            data_type = DataTypeFactory::instance().create_data_type(it->second, true);
        }
        if (data_type == nullptr) {
            return Status::NotSupported("Not support data type {} for column {}",
                                        it == _name_to_col_type.end() ? slot->type().debug_string()
                                                                      : it->second.debug_string(),
                                        slot->col_name());
        }
        MutableColumnPtr data_column = data_type->create_column();
        _src_block.insert(
                ColumnWithTypeAndName(std::move(data_column), data_type, slot->col_name()));
        _src_block_name_to_idx.emplace(slot->col_name(), idx++);
    }
    _src_block_ptr = &_src_block;
    _src_block_init = true;
    return Status::OK();
}

Status VFileScanner::_cast_to_input_block(Block* block) {
    if (!_is_load) {
        return Status::OK();
    }
    if (_is_dynamic_schema) {
        return Status::OK();
    }
    SCOPED_TIMER(_cast_to_input_block_timer);
    // cast primitive type(PT0) to primitive type(PT1)
    size_t idx = 0;
    for (auto& slot_desc : _input_tuple_desc->slots()) {
        if (_name_to_col_type.find(slot_desc->col_name()) == _name_to_col_type.end()) {
            // skip columns which does not exist in file
            continue;
        }
        if (slot_desc->type().is_variant_type()) {
            // skip variant type
            continue;
        }
        auto& arg = _src_block_ptr->get_by_name(slot_desc->col_name());
        // remove nullable here, let the get_function decide whether nullable
        auto return_type = slot_desc->get_data_type_ptr();
        ColumnsWithTypeAndName arguments {
                arg,
                {DataTypeString().create_column_const(
                         arg.column->size(), remove_nullable(return_type)->get_family_name()),
                 std::make_shared<DataTypeString>(), ""}};
        auto func_cast =
                SimpleFunctionFactory::instance().get_function("CAST", arguments, return_type);
        idx = _src_block_name_to_idx[slot_desc->col_name()];
        RETURN_IF_ERROR(
                func_cast->execute(nullptr, *_src_block_ptr, {idx}, idx, arg.column->size()));
        _src_block_ptr->get_by_position(idx).type = std::move(return_type);
    }
    return Status::OK();
}

Status VFileScanner::_fill_columns_from_path(size_t rows) {
    const TFileRangeDesc& range = _ranges.at(_next_range - 1);
    if (range.__isset.columns_from_path && !_partition_slot_descs.empty()) {
        SCOPED_TIMER(_fill_path_columns_timer);
        for (const auto& slot_desc : _partition_slot_descs) {
            if (slot_desc == nullptr) continue;
            auto it = _partition_slot_index_map.find(slot_desc->id());
            if (it == std::end(_partition_slot_index_map)) {
                std::stringstream ss;
                ss << "Unknown source slot descriptor, slot_id=" << slot_desc->id();
                return Status::InternalError(ss.str());
            }
            const std::string& column_from_path = range.columns_from_path[it->second];
            auto doris_column = _src_block_ptr->get_by_name(slot_desc->col_name()).column;
            IColumn* col_ptr = const_cast<IColumn*>(doris_column.get());

            if (!_text_converter->write_vec_column(slot_desc, col_ptr,
                                                   const_cast<char*>(column_from_path.c_str()),
                                                   column_from_path.size(), true, false, rows)) {
                return Status::InternalError("Failed to fill partition column: {}={}",
                                             slot_desc->col_name(), column_from_path);
            }
        }
    }
    return Status::OK();
}

Status VFileScanner::_fill_missing_columns(size_t rows) {
    if (_missing_cols.empty()) {
        return Status::OK();
    }

    SCOPED_TIMER(_fill_missing_columns_timer);
    for (auto slot_desc : _real_tuple_desc->slots()) {
        if (!slot_desc->is_materialized()) {
            continue;
        }
        if (_missing_cols.find(slot_desc->col_name()) == _missing_cols.end()) {
            continue;
        }

        auto it = _col_default_value_ctx.find(slot_desc->col_name());
        if (it == _col_default_value_ctx.end()) {
            return Status::InternalError("failed to find default value expr for slot: {}",
                                         slot_desc->col_name());
        }
        if (it->second == nullptr) {
            // no default column, fill with null
            auto nullable_column = reinterpret_cast<vectorized::ColumnNullable*>(
                    (*std::move(_src_block_ptr->get_by_name(slot_desc->col_name()).column))
                            .mutate()
                            .get());
            nullable_column->insert_many_defaults(rows);
        } else {
            // fill with default value
            auto& ctx = it->second;
            auto origin_column_num = _src_block_ptr->columns();
            int result_column_id = -1;
            // PT1 => dest primitive type
            RETURN_IF_ERROR(ctx->execute(_src_block_ptr, &result_column_id));
            bool is_origin_column = result_column_id < origin_column_num;
            if (!is_origin_column) {
                // call resize because the first column of _src_block_ptr may not be filled by reader,
                // so _src_block_ptr->rows() may return wrong result, cause the column created by `ctx->execute()`
                // has only one row.
                std::move(*_src_block_ptr->get_by_position(result_column_id).column)
                        .mutate()
                        ->resize(rows);
                auto result_column_ptr = _src_block_ptr->get_by_position(result_column_id).column;
                // result_column_ptr maybe a ColumnConst, convert it to a normal column
                result_column_ptr = result_column_ptr->convert_to_full_column_if_const();
                auto origin_column_type = _src_block_ptr->get_by_name(slot_desc->col_name()).type;
                bool is_nullable = origin_column_type->is_nullable();
                _src_block_ptr->replace_by_position(
                        _src_block_ptr->get_position_by_name(slot_desc->col_name()),
                        is_nullable ? make_nullable(result_column_ptr) : result_column_ptr);
                _src_block_ptr->erase(result_column_id);
            }
        }
    }
    return Status::OK();
}

Status VFileScanner::_pre_filter_src_block() {
    if (!_is_load) {
        return Status::OK();
    }
    if (!_pre_conjunct_ctxs.empty()) {
        SCOPED_TIMER(_pre_filter_timer);
        auto origin_column_num = _src_block_ptr->columns();
        auto old_rows = _src_block_ptr->rows();
        RETURN_IF_ERROR(vectorized::VExprContext::filter_block(_pre_conjunct_ctxs, _src_block_ptr,
                                                               origin_column_num));
        _counter.num_rows_unselected += old_rows - _src_block.rows();
    }
    return Status::OK();
}

Status VFileScanner::_convert_to_output_block(Block* block) {
    if (!_is_load) {
        return Status::OK();
    }

    SCOPED_TIMER(_convert_to_output_block_timer);
    // The block is passed from scanner context's free blocks,
    // which is initialized by src columns.
    // But for load job, the block should be filled with dest columns.
    // So need to clear it first.
    block->clear();

    int ctx_idx = 0;
    size_t rows = _src_block.rows();
    auto filter_column = vectorized::ColumnUInt8::create(rows, 1);
    auto& filter_map = filter_column->get_data();

    for (auto slot_desc : _output_tuple_desc->slots()) {
        if (!slot_desc->is_materialized()) {
            continue;
        }
        int dest_index = ctx_idx++;
        vectorized::ColumnPtr column_ptr;

        auto& ctx = _dest_vexpr_ctx[dest_index];
        int result_column_id = -1;
        // PT1 => dest primitive type
        RETURN_IF_ERROR(ctx->execute(&_src_block, &result_column_id));
        column_ptr = _src_block.get_by_position(result_column_id).column;
        // column_ptr maybe a ColumnConst, convert it to a normal column
        column_ptr = column_ptr->convert_to_full_column_if_const();
        DCHECK(column_ptr != nullptr);

        // because of src_slot_desc is always be nullable, so the column_ptr after do dest_expr
        // is likely to be nullable
        if (LIKELY(column_ptr->is_nullable())) {
            const ColumnNullable* nullable_column =
                    reinterpret_cast<const vectorized::ColumnNullable*>(column_ptr.get());
            for (int i = 0; i < rows; ++i) {
                if (filter_map[i] && nullable_column->is_null_at(i)) {
                    if (_strict_mode && (_src_slot_descs_order_by_dest[dest_index]) &&
                        !_src_block.get_by_position(_dest_slot_to_src_slot_index[dest_index])
                                 .column->is_null_at(i)) {
                        RETURN_IF_ERROR(_state->append_error_msg_to_file(
                                [&]() -> std::string {
                                    return _src_block.dump_one_line(i, _num_of_columns_from_file);
                                },
                                [&]() -> std::string {
                                    auto raw_value =
                                            _src_block.get_by_position(ctx_idx).column->get_data_at(
                                                    i);
                                    std::string raw_string = raw_value.to_string();
                                    fmt::memory_buffer error_msg;
                                    fmt::format_to(error_msg,
                                                   "column({}) value is incorrect while strict "
                                                   "mode is {}, "
                                                   "src value is {}",
                                                   slot_desc->col_name(), _strict_mode, raw_string);
                                    return fmt::to_string(error_msg);
                                },
                                &_scanner_eof));
                        filter_map[i] = false;
                    } else if (!slot_desc->is_nullable()) {
                        RETURN_IF_ERROR(_state->append_error_msg_to_file(
                                [&]() -> std::string {
                                    return _src_block.dump_one_line(i, _num_of_columns_from_file);
                                },
                                [&]() -> std::string {
                                    fmt::memory_buffer error_msg;
                                    fmt::format_to(error_msg,
                                                   "column({}) values is null while columns is not "
                                                   "nullable",
                                                   slot_desc->col_name());
                                    return fmt::to_string(error_msg);
                                },
                                &_scanner_eof));
                        filter_map[i] = false;
                    }
                }
            }
            if (!slot_desc->is_nullable()) {
                column_ptr = remove_nullable(column_ptr);
            }
        } else if (slot_desc->is_nullable()) {
            column_ptr = make_nullable(column_ptr);
        }
        block->insert(dest_index, vectorized::ColumnWithTypeAndName(std::move(column_ptr),
                                                                    slot_desc->get_data_type_ptr(),
                                                                    slot_desc->col_name()));
    }

    // after do the dest block insert operation, clear _src_block to remove the reference of origin column
    _src_block.clear();

    size_t dest_size = block->columns();
    // do filter
    block->insert(vectorized::ColumnWithTypeAndName(std::move(filter_column),
                                                    std::make_shared<vectorized::DataTypeUInt8>(),
                                                    "filter column"));
    RETURN_IF_ERROR(vectorized::Block::filter_block(block, dest_size, dest_size));

    _counter.num_rows_filtered += rows - block->rows();
    return Status::OK();
}

Status VFileScanner::_get_next_reader() {
    while (true) {
        _cur_reader.reset(nullptr);
        _src_block_init = false;
        if (_next_range >= _ranges.size()) {
            _scanner_eof = true;
            _state->update_num_finished_scan_range(1);
            return Status::OK();
        }
        if (_next_range != 0) {
            _state->update_num_finished_scan_range(1);
        }

        const TFileRangeDesc& range = _ranges[_next_range++];

        // create reader for specific format
        // TODO: add json, avro
        Status init_status;
        // TODO: use data lake type
        switch (_params.format_type) {
        case TFileFormatType::FORMAT_PARQUET: {
            std::unique_ptr<ParquetReader> parquet_reader = ParquetReader::create_unique(
                    _profile, _params, range, _state->query_options().batch_size,
                    const_cast<cctz::time_zone*>(&_state->timezone_obj()), _io_ctx.get(), _state,
                    _kv_cache, _state->query_options().enable_parquet_lazy_mat);
            RETURN_IF_ERROR(parquet_reader->open());
            if (!_is_load && _push_down_conjuncts.empty() && !_conjuncts.empty()) {
                _push_down_conjuncts.resize(_conjuncts.size());
                for (size_t i = 0; i != _conjuncts.size(); ++i) {
                    RETURN_IF_ERROR(_conjuncts[i]->clone(_state, _push_down_conjuncts[i]));
                }
                _discard_conjuncts();
            }
            if (range.__isset.table_format_params &&
                range.table_format_params.table_format_type == "iceberg") {
                std::unique_ptr<IcebergTableReader> iceberg_reader =
                        IcebergTableReader::create_unique(std::move(parquet_reader), _profile,
                                                          _state, _params, range, _kv_cache,
                                                          _io_ctx.get());
                init_status = iceberg_reader->init_reader(
                        _file_col_names, _col_id_name_map, _colname_to_value_range,
                        _push_down_conjuncts, _real_tuple_desc, _default_val_row_desc.get(),
                        _col_name_to_slot_id, &_not_single_slot_filter_conjuncts,
                        &_slot_id_to_filter_conjuncts);
                RETURN_IF_ERROR(iceberg_reader->init_row_filters(range));
                _cur_reader = std::move(iceberg_reader);
            } else {
                std::vector<std::string> place_holder;
                init_status = parquet_reader->init_reader(
                        _file_col_names, place_holder, _colname_to_value_range,
                        _push_down_conjuncts, _real_tuple_desc, _default_val_row_desc.get(),
                        _col_name_to_slot_id, &_not_single_slot_filter_conjuncts,
                        &_slot_id_to_filter_conjuncts);
                _cur_reader = std::move(parquet_reader);
            }
            break;
        }
        case TFileFormatType::FORMAT_ORC: {
            if (!_is_load && _push_down_conjuncts.empty() && !_conjuncts.empty()) {
                _push_down_conjuncts.resize(_conjuncts.size());
                for (size_t i = 0; i != _conjuncts.size(); ++i) {
                    RETURN_IF_ERROR(_conjuncts[i]->clone(_state, _push_down_conjuncts[i]));
                }
                _discard_conjuncts();
            }
            _cur_reader = OrcReader::create_unique(
                    _profile, _state, _params, range, _file_col_names,
                    _state->query_options().batch_size, _state->timezone(), _io_ctx.get(),
                    _state->query_options().enable_orc_lazy_mat);
            init_status = ((OrcReader*)(_cur_reader.get()))
                                  ->init_reader(_colname_to_value_range, _push_down_conjuncts);
            break;
        }
        case TFileFormatType::FORMAT_CSV_PLAIN:
        case TFileFormatType::FORMAT_CSV_GZ:
        case TFileFormatType::FORMAT_CSV_BZ2:
        case TFileFormatType::FORMAT_CSV_LZ4FRAME:
        case TFileFormatType::FORMAT_CSV_LZOP:
        case TFileFormatType::FORMAT_CSV_DEFLATE:
        case TFileFormatType::FORMAT_PROTO: {
            _cur_reader = CsvReader::create_unique(_state, _profile, &_counter, _params, range,
                                                   _file_slot_descs, _io_ctx.get());
            init_status = ((CsvReader*)(_cur_reader.get()))->init_reader(_is_load);
            break;
        }
        case TFileFormatType::FORMAT_JSON: {
            _cur_reader = NewJsonReader::create_unique(_state, _profile, &_counter, _params, range,
                                                       _file_slot_descs, &_scanner_eof,
                                                       _io_ctx.get(), _is_dynamic_schema);
            init_status = ((NewJsonReader*)(_cur_reader.get()))->init_reader();
            break;
        }
        default:
            return Status::InternalError("Not supported file format: {}", _params.format_type);
        }

        if (init_status.is<END_OF_FILE>()) {
            COUNTER_UPDATE(_empty_file_counter, 1);
            continue;
        } else if (!init_status.ok()) {
            if (init_status.is<ErrorCode::NOT_FOUND>()) {
                COUNTER_UPDATE(_empty_file_counter, 1);
                LOG(INFO) << "failed to find file: " << range.path;
                return init_status;
            }
            return Status::InternalError("failed to init reader for file {}, err: {}", range.path,
                                         init_status.to_string());
        }

        _name_to_col_type.clear();
        _missing_cols.clear();
        _cur_reader->get_columns(&_name_to_col_type, &_missing_cols);
        RETURN_IF_ERROR(_generate_fill_columns());
        if (VLOG_NOTICE_IS_ON && !_missing_cols.empty() && _is_load) {
            fmt::memory_buffer col_buf;
            for (auto& col : _missing_cols) {
                fmt::format_to(col_buf, " {}", col);
            }
            VLOG_NOTICE << fmt::format("Unknown columns:{} in file {}", fmt::to_string(col_buf),
                                       range.path);
        }
        _cur_reader_eof = false;
        break;
    }
    return Status::OK();
}

Status VFileScanner::_generate_fill_columns() {
    std::unordered_map<std::string, std::tuple<std::string, const SlotDescriptor*>>
            partition_columns;
    std::unordered_map<std::string, VExprContextSPtr> missing_columns;

    const TFileRangeDesc& range = _ranges.at(_next_range - 1);
    if (range.__isset.columns_from_path && !_partition_slot_descs.empty()) {
        for (const auto& slot_desc : _partition_slot_descs) {
            if (slot_desc) {
                auto it = _partition_slot_index_map.find(slot_desc->id());
                if (it == std::end(_partition_slot_index_map)) {
                    return Status::InternalError("Unknown source slot descriptor, slot_id={}",
                                                 slot_desc->id());
                }
                const std::string& column_from_path = range.columns_from_path[it->second];
                partition_columns.emplace(slot_desc->col_name(),
                                          std::make_tuple(column_from_path, slot_desc));
            }
        }
    }

    if (!_missing_cols.empty()) {
        for (auto slot_desc : _real_tuple_desc->slots()) {
            if (!slot_desc->is_materialized()) {
                continue;
            }
            if (_missing_cols.find(slot_desc->col_name()) == _missing_cols.end()) {
                continue;
            }

            auto it = _col_default_value_ctx.find(slot_desc->col_name());
            if (it == _col_default_value_ctx.end()) {
                return Status::InternalError("failed to find default value expr for slot: {}",
                                             slot_desc->col_name());
            }
            missing_columns.emplace(slot_desc->col_name(), it->second);
        }
    }

    return _cur_reader->set_fill_columns(partition_columns, missing_columns);
}

Status VFileScanner::_init_expr_ctxes() {
    DCHECK(!_ranges.empty());

    std::map<SlotId, int> full_src_index_map;
    std::map<SlotId, SlotDescriptor*> full_src_slot_map;
    std::map<std::string, int> partition_name_to_key_index_map;
    int index = 0;
    for (const auto& slot_desc : _real_tuple_desc->slots()) {
        full_src_slot_map.emplace(slot_desc->id(), slot_desc);
        full_src_index_map.emplace(slot_desc->id(), index++);
    }

    // For external table query, find the index of column in path.
    // Because query doesn't always search for all columns in a table
    // and the order of selected columns is random.
    // All ranges in _ranges vector should have identical columns_from_path_keys
    // because they are all file splits for the same external table.
    // So here use the first element of _ranges to fill the partition_name_to_key_index_map
    if (_ranges[0].__isset.columns_from_path_keys) {
        std::vector<std::string> key_map = _ranges[0].columns_from_path_keys;
        if (!key_map.empty()) {
            for (size_t i = 0; i < key_map.size(); i++) {
                partition_name_to_key_index_map.emplace(key_map[i], i);
            }
        }
    }

    _num_of_columns_from_file = _params.num_of_columns_from_file;
    for (const auto& slot_info : _params.required_slots) {
        auto slot_id = slot_info.slot_id;
        auto it = full_src_slot_map.find(slot_id);
        if (it == std::end(full_src_slot_map)) {
            std::stringstream ss;
            ss << "Unknown source slot descriptor, slot_id=" << slot_id;
            return Status::InternalError(ss.str());
        }
        if (slot_info.is_file_slot) {
            _file_slot_descs.emplace_back(it->second);
            _file_col_names.push_back(it->second->col_name());
            if (it->second->col_unique_id() > 0) {
                _col_id_name_map.emplace(it->second->col_unique_id(), it->second->col_name());
            }
        } else {
            _partition_slot_descs.emplace_back(it->second);
            if (_is_load) {
                auto iti = full_src_index_map.find(slot_id);
                _partition_slot_index_map.emplace(slot_id, iti->second - _num_of_columns_from_file);
            } else {
                auto kit = partition_name_to_key_index_map.find(it->second->col_name());
                _partition_slot_index_map.emplace(slot_id, kit->second);
            }
        }
    }

    // set column name to default value expr map
    for (auto slot_desc : _real_tuple_desc->slots()) {
        if (!slot_desc->is_materialized()) {
            continue;
        }
        vectorized::VExprContextSPtr ctx;
        auto it = _params.default_value_of_src_slot.find(slot_desc->id());
        if (it != std::end(_params.default_value_of_src_slot)) {
            if (!it->second.nodes.empty()) {
                RETURN_IF_ERROR(vectorized::VExpr::create_expr_tree(it->second, ctx));
                RETURN_IF_ERROR(ctx->prepare(_state, *_default_val_row_desc));
                RETURN_IF_ERROR(ctx->open(_state));
            }
            // if expr is empty, the default value will be null
            _col_default_value_ctx.emplace(slot_desc->col_name(), ctx);
        }
    }

    if (_is_load) {
        // follow desc expr map is only for load task.
        bool has_slot_id_map = _params.__isset.dest_sid_to_src_sid_without_trans;
        int idx = 0;
        for (auto slot_desc : _output_tuple_desc->slots()) {
            if (!slot_desc->is_materialized()) {
                continue;
            }
            auto it = _params.expr_of_dest_slot.find(slot_desc->id());
            if (it == std::end(_params.expr_of_dest_slot)) {
                return Status::InternalError("No expr for dest slot, id={}, name={}",
                                             slot_desc->id(), slot_desc->col_name());
            }

            vectorized::VExprContextSPtr ctx;
            if (!it->second.nodes.empty()) {
                RETURN_IF_ERROR(vectorized::VExpr::create_expr_tree(it->second, ctx));
                RETURN_IF_ERROR(ctx->prepare(_state, *_src_row_desc));
                RETURN_IF_ERROR(ctx->open(_state));
            }
            _dest_vexpr_ctx.emplace_back(ctx);
            _dest_slot_name_to_idx[slot_desc->col_name()] = idx++;

            if (has_slot_id_map) {
                auto it1 = _params.dest_sid_to_src_sid_without_trans.find(slot_desc->id());
                if (it1 == std::end(_params.dest_sid_to_src_sid_without_trans)) {
                    _src_slot_descs_order_by_dest.emplace_back(nullptr);
                } else {
                    auto _src_slot_it = full_src_slot_map.find(it1->second);
                    if (_src_slot_it == std::end(full_src_slot_map)) {
                        return Status::InternalError("No src slot {} in src slot descs",
                                                     it1->second);
                    }
                    _dest_slot_to_src_slot_index.emplace(_src_slot_descs_order_by_dest.size(),
                                                         full_src_index_map[_src_slot_it->first]);
                    _src_slot_descs_order_by_dest.emplace_back(_src_slot_it->second);
                }
            }
        }
    }
    // If last slot is_variant from stream plan which indicate table is dynamic schema
    _is_dynamic_schema =
            _output_tuple_desc && _output_tuple_desc->slots().back()->type().is_variant_type();

    // TODO: It should can move to scan node to process.
    if (!_conjuncts.empty()) {
        _split_conjuncts();
    }
    return Status::OK();
}

Status VFileScanner::close(RuntimeState* state) {
    if (_is_closed) {
        return Status::OK();
    }

    for (auto ctx : _dest_vexpr_ctx) {
        if (ctx != nullptr) {
            ctx->close(state);
        }
    }

    for (auto& it : _col_default_value_ctx) {
        if (it.second != nullptr) {
            it.second->close(state);
        }
    }

    for (auto& conjunct : _pre_conjunct_ctxs) {
        conjunct->close(state);
    }

    for (auto& conjunct : _push_down_conjuncts) {
        conjunct->close(state);
    }

    for (auto& [k, v] : _slot_id_to_filter_conjuncts) {
        for (auto& ctx : v) {
            if (ctx != nullptr) {
                ctx->close(state);
            }
        }
    }

    for (auto ctx : _not_single_slot_filter_conjuncts) {
        if (ctx != nullptr) {
            ctx->close(state);
        }
    }

    if (config::enable_file_cache && _state->query_options().enable_file_cache) {
        io::FileCacheProfileReporter cache_profile(_profile);
        cache_profile.update(_file_cache_statistics.get());
    }

    RETURN_IF_ERROR(VScanner::close(state));
    return Status::OK();
}

} // namespace doris::vectorized
