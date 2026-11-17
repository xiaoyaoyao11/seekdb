#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace oceanbase { namespace sql {
struct FileSqlSchema;
struct FileSqlRow;
// A borrowed-fd, page-streaming reader. The generic file reader owns secure open,
// fingerprint verification and descriptor lifetime. This class never closes fd.
class FileSqlParquetReader {
public:
  FileSqlParquetReader();
  ~FileSqlParquetReader();
  int infer(int fd, FileSqlSchema &);
  int open(int fd, const FileSqlSchema &, const std::vector<int64_t> &projection);
  int next(FileSqlRow &);
  int rescan();
  void close();
  const std::string &error() const;
private:
  FileSqlParquetReader(const FileSqlParquetReader &) = delete;
  FileSqlParquetReader &operator=(const FileSqlParquetReader &) = delete;
  struct Impl;
  Impl *impl_;
};
} }
