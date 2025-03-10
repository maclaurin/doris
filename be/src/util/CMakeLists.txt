# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

# where to put generated libraries
set(LIBRARY_OUTPUT_PATH "${BUILD_DIR}/src/util")

# where to put generated binaries
set(EXECUTABLE_OUTPUT_PATH "${BUILD_DIR}/src/util")

add_library(cityhash cityhash102/city.cc)

set(UTIL_FILES
  arrow/row_batch.cpp
  arrow/utils.cpp
  arrow/block_convertor.cpp
  bfd_parser.cpp
  bitmap.cpp
  block_compression.cpp
  coding.cpp
  cpu_info.cpp
  crc32c.cpp
  date_func.cpp
  dynamic_util.cpp
  debug_util.cpp
  disk_info.cpp
  errno.cpp
  hash_util.hpp
  histogram.cpp
  doris_metrics.cpp
  mem_info.cpp
  metrics.cpp
  murmur_hash3.cpp
  network_util.cpp
  parse_util.cpp
  path_builder.cpp
# TODO: not supported on RHEL 5
  perf_counters.cpp
  runtime_profile.cpp
  static_asserts.cpp
  thrift_util.cpp
  thrift_client.cpp
  thrift_server.cpp
  stack_util.cpp
  system_metrics.cpp
  url_parser.cpp
  url_coding.cpp
  mysql_row_buffer.cpp
  error_util.cc
  time.cpp
  os_info.cpp
  os_util.cpp
  # coding_util.cpp
  cidr.cpp
  core_local.cpp
  uid_util.cpp
  encryption_util.cpp
  string_util.cpp
  md5.cpp
  sm3.cpp
  thrift_rpc_helper.cpp
  faststring.cc
  slice.cpp
  frame_of_reference_coding.cpp
  utf8_check.cpp
  cgroup_util.cpp
  path_util.cpp
  thread.cpp
  threadpool.cpp
  trace.cpp
  trace_metrics.cpp
  timezone_utils.cpp
  easy_json.cc
  mustache/mustache.cc
  brpc_client_cache.cpp
  zlib.cpp
  pprof_utils.cpp
  s3_uri.cpp
  s3_util.cpp
  hdfs_util.cpp
  time_lut.cpp
  telemetry/telemetry.cpp
  telemetry/brpc_carrier.cpp
  telemetry/open_telemetry_scop_wrapper.hpp
  quantile_state.cpp
  jni-util.cpp
  libjvm_loader.cpp
  jni_native_method.cpp
)


if (OS_MACOSX)
    list(REMOVE_ITEM UTIL_FILES perf_counters.cpp disk_info.cpp)
    list(APPEND UTIL_FILES perf_counters_mac.cpp disk_info_mac.cpp)
endif()

add_library(Util STATIC
    ${UTIL_FILES}
)
target_link_libraries(Util cityhash)

pch_reuse(Util)