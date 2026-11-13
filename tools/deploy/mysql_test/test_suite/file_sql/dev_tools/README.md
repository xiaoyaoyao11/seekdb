# Development tools of the file as database feature

The files of this directory are **development tools**, they are not part of
the build of the server and they are not collected by the unit test tree.
The unit test tree of OceanBase collects every `unittest/**/*.cpp` file as a
test source, a program with its own `main()` would therefore break the link
of the unit tests.  The tools are kept here for that reason, they are plain
programs with a `main()` that a developer compiles and runs by hand.

## The tools

| File | Purpose |
| --- | --- |
| `copy_writer_test.cpp` | Checks the CSV writer of the COPY statement: the header, the quoting of the fields, the representation of the NULL values, the refusal to overwrite an existing file and the removal of a half written file after a failed export. |
| `copy_roundtrip_test.cpp` | Checks the round trip of the COPY statement: the file that the writer produces must be read back by the reader of the `read_csv()` table function, with and without a configured NULL text. |
| `check_outfile_equivalence.cpp` | Checks that the changes of the COPY statement do not change the behaviour of an ordinary `SELECT ... INTO OUTFILE`.  The tool compares the original source of the operator with the changed one, every line that the change removed or added must belong to the branch of the COPY statement.  The four lines that the change removed are the ones that the tool reports as equivalent changes. |
| `bison3_compat.ps1` | Rewrites the grammar of the OceanBase parser into a form that a modern bison accepts, so that a conflict analysis of the grammar becomes possible. |
| `bison3_compat.py` | The same rewrite for the environments that have a python interpreter. |

## How they are used

The programs are compiled against the sources of the server, they need the
include directory of the source tree:

```sh
# the writer and its round trip
g++ -std=c++17 -I src -o copy_writer_test.exe \
    tools/deploy/mysql_test/test_suite/file_sql/dev_tools/copy_writer_test.cpp \
    src/sql/engine/basic/ob_file_copy_writer.cpp \
    src/sql/engine/basic/ob_file_scan_utils.cpp
./copy_writer_test.exe

g++ -std=c++17 -I src -o copy_roundtrip_test.exe \
    tools/deploy/mysql_test/test_suite/file_sql/dev_tools/copy_roundtrip_test.cpp \
    src/sql/engine/basic/ob_file_copy_writer.cpp \
    src/sql/engine/basic/ob_file_scan_utils.cpp
./copy_roundtrip_test.exe
```

The check of the ordinary `SELECT ... INTO OUTFILE` compares the source of
the operator before the change with the source after the change:

```sh
# the original source of the operator
git show HEAD:src/sql/engine/basic/ob_select_into_op.cpp > /tmp/ob_select_into_op_ORIGINAL.cpp

g++ -std=c++17 -o check_outfile_equivalence.exe \
    tools/deploy/mysql_test/test_suite/file_sql/dev_tools/check_outfile_equivalence.cpp
./check_outfile_equivalence.exe /tmp/ob_select_into_op_ORIGINAL.cpp \
    src/sql/engine/basic/ob_select_into_op.cpp
```

The tool prints the lines that the change removed together with the reason
why they are equivalent, and it prints `THE ORDINARY SELECT ... INTO OUTFILE
IS UNCHANGED` when the ordinary path was not touched.

The conflict analysis of the grammar needs a bison that is newer than the
version that the build of the server uses, the build of the server uses
bison 2.4.1 and the grammar uses the mid rule references that the newer
versions of bison reject:

```sh
# the rewrite, the output is a grammar that bison 3 accepts
powershell -File tools/deploy/mysql_test/test_suite/file_sql/dev_tools/bison3_compat.ps1 \
    -InFile src/sql/parser/sql_parser_mysql_mode.y \
    -OutFile /tmp/sql_parser_mysql_mode_bison3.y

# the conflict analysis, the report of bison holds the conflicts
bison -v -Wno-deprecated -Wno-other -d \
    -o /tmp/sql_parser_mysql_mode_tab.c /tmp/sql_parser_mysql_mode_bison3.y
grep conflict /tmp/sql_parser_mysql_mode_tab.output
```

The grammar of the server must never have a conflict, the build of the
parser fails when it has one.

## The unit tests

The unit tests of the feature live in the unit test tree of the server, they
are the normative tests:

- `unittest/sql/engine/basic/test_file_copy_writer.cpp` — the CSV writer.
- `unittest/sql/engine/basic/test_parquet_reader.cpp` — the Parquet reader.
