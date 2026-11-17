#pragma once

#include "sql/engine/basic/file_sql_reader.h"
#include "lib/string/ob_string.h"
#include "lib/ob_errno.h"
#include <cstring>
#include <limits>

namespace oceanbase { namespace sql {
// Immutable, versioned plan descriptor. No file rows or open handles enter a plan.
// Length-prefixed little-endian fields avoid delimiter/escaping ambiguities.
struct FileSqlPlan {
  std::string path;
  FileSqlOptions options;
  FileSqlSchema schema;
  static void put(std::string &s, uint64_t v) {
    for (int i=0;i<8;++i) { s.push_back(static_cast<char>(v & 255)); v >>= 8; }
  }
  static void put_string(std::string &s, const std::string &v) {
    put(s, v.size()); s.append(v);
  }
  std::string encode() const {
    std::string s("FSQL1");
    put_string(s,path); put_string(s,options.format);
    put(s,options.header); put(s,static_cast<unsigned char>(options.delimiter));
    put(s,static_cast<unsigned char>(options.quote)); put(s,static_cast<unsigned char>(options.escape));
    put_string(s,options.nullstr); put(s,options.sample_size);
    const auto &f=schema.fingerprint;
    put(s,f.device); put(s,f.inode); put(s,f.size); put(s,f.mtime_sec); put(s,f.mtime_nsec);
    put(s,f.ctime_sec); put(s,f.ctime_nsec); put(s,schema.row_count); put(s,schema.columns.size());
    for (const auto &c:schema.columns) { put_string(s,c.name); put(s,static_cast<uint64_t>(c.type)|(static_cast<uint64_t>(c.scale)<<8)); put(s,c.nullable); }
    return s;
  }
  int decode(const common::ObString &s) {
    const char *p=s.ptr(); int64_t n=s.length();
    if (n<5 || p==nullptr || std::memcmp(p,"FSQL1",5)!=0) return common::OB_INVALID_ARGUMENT;
    p+=5; n-=5; bool ok=true;
    auto get=[&]() -> uint64_t {
      if (n<8) { ok=false; return 0; }
      uint64_t v=0; for (int i=0;i<8;++i) v|=static_cast<uint64_t>(static_cast<unsigned char>(p[i]))<<(8*i);
      p+=8; n-=8; return v;
    };
    auto str=[&](std::string &v) {
      uint64_t z=get();
      if (!ok || z>static_cast<uint64_t>(n)) { ok=false; return; }
      v.assign(p,z); p+=z; n-=z;
    };
    str(path); str(options.format); options.header=get(); options.delimiter=get(); options.quote=get(); options.escape=get();
    str(options.nullstr); options.sample_size=static_cast<int64_t>(get());
    auto &f=schema.fingerprint;
    f.device=get(); f.inode=get(); f.size=get(); f.mtime_sec=get(); f.mtime_nsec=get(); f.ctime_sec=get(); f.ctime_nsec=get();
    schema.row_count=get(); const uint64_t count=get();
    if (!ok || count>4096) return common::OB_INVALID_ARGUMENT;
    schema.columns.clear();
    for (uint64_t i=0;ok && i<count;++i) {
      FileSqlColumn c; str(c.name); const uint64_t type=get(); c.nullable=get();
      if ((type&255)>static_cast<uint64_t>(FileSqlType::DATETIME) || (type>>8)>6) ok=false;
      c.type=static_cast<FileSqlType>(type&255);c.scale=static_cast<int>(type>>8); schema.columns.push_back(c);
    }
    return ok && n==0 ? common::OB_SUCCESS : common::OB_INVALID_ARGUMENT;
  }
};
} }
