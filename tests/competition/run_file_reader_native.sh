#!/usr/bin/env bash
set -eu
repo="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
deps="${FILE_SQL_DEPS_INCLUDE:-/root/oceanbase2026/seekdb/deps/3rd/usr/local/oceanbase/deps/devel/include}"
work="${FILE_SQL_TEST_WORK:-/root/oceanbase2026/file-reader-native}"
mkdir -p "$work"
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror -O1 -g \
  -I"$repo/src" -I"$repo/src/oblib" -isystem "$deps" \
  "$repo/tests/competition/test_file_reader_native.cpp" \
  "$repo/src/sql/engine/basic/file_sql_reader.cpp" \
  "$repo/src/sql/engine/basic/file_sql_parquet.cpp" -o "$work/test_file_reader_native"
"$work/test_file_reader_native"
