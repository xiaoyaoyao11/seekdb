#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace oceanbase { namespace sql {
enum class FileSqlType { NULL_VALUE, BIGINT, DOUBLE, BOOLEAN, VARCHAR, DATE, DATETIME };
struct FileSqlColumn { std::string name; FileSqlType type; bool nullable; int scale=0; };
struct FileSqlOptions {
  std::string format;
  bool header = true;
  char delimiter = ',';
  char quote = '"';
  char escape = '"';
  std::string nullstr = "NULL";
  int64_t sample_size = -1;
};
struct FileSqlFingerprint {
  uint64_t device=0,inode=0,size=0;
  int64_t mtime_sec=0,mtime_nsec=0,ctime_sec=0,ctime_nsec=0;
};
struct FileSqlSchema {
  std::vector<FileSqlColumn> columns;
  FileSqlFingerprint fingerprint;
  int64_t row_count=0;
};
struct FileSqlCell { bool is_null=true; std::string text; };
struct FileSqlRow { std::vector<FileSqlCell> cells; };
// Full schema inference, bounded single-record streaming scan. Not thread-safe.
// Limits: 4096 columns, 8 MiB/record, 16 MiB schema text, 65536 glob entries.
class FileSqlReader {
public:
  FileSqlReader();
  ~FileSqlReader();
  int infer(const std::string &path, const FileSqlOptions&, const std::string &secure_root, FileSqlSchema &out);
  int open(const std::string &path, const FileSqlOptions&, const std::string &secure_root,
           const FileSqlSchema &expected, const std::vector<int64_t> &projection);
  int next(FileSqlRow&);
  int rescan();
  void close();
  const std::string &error() const;
private:
  FileSqlReader(const FileSqlReader&) = delete;
  FileSqlReader &operator=(const FileSqlReader&) = delete;
  struct Impl;
  Impl *impl_;
};
} }
