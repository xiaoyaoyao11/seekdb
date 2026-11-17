#!/usr/bin/env bash
set -eu
repo="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
deps="${FILE_SQL_DEPS_INCLUDE:-/root/oceanbase2026/seekdb/deps/3rd/usr/local/oceanbase/deps/devel/include}"
work="${FILE_SQL_TEST_WORK:-/root/oceanbase2026/file-copy-native}"
mkdir -p "$work"
"${CXX:-g++}" -std=c++17 -Wall -Wextra -O1 -g \
  -I"$repo/src" -I"$repo/src/oblib" -isystem "$deps" \
  "$repo/tests/competition/test_file_copy_native.cpp" \
  -Wl,--wrap=fsync -Wl,--wrap=linkat -o "$work/test_file_copy_native"
# Remove all capabilities: descriptor publication must not depend on
# CAP_DAC_READ_SEARCH (which Linux AT_EMPTY_PATH normally requires).
fixtures="$(mktemp -d "$work/fixtures.XXXXXX")"
trap 'rm -rf -- "$fixtures"' EXIT
if [ "$(id -u)" = 0 ]; then
  setpriv --bounding-set=-all --inh-caps=-all --ambient-caps=-all "$work/test_file_copy_native" "$fixtures"
else
  "$work/test_file_copy_native" "$fixtures"
fi
