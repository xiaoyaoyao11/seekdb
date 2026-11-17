#pragma once
#include "sql/engine/basic/file_sql_reader.h"
#include "lib/ob_errno.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <locale>
#include <new>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <fnmatch.h>
#include <functional>
#include <map>
#include <memory>

namespace oceanbase { namespace sql { namespace file_sql_detail {
using namespace common;
constexpr size_t MAX_COLUMNS = 4096;
constexpr size_t MAX_RECORD = 8 * 1024 * 1024;
constexpr size_t MAX_METADATA = 16 * 1024 * 1024;
inline std::string lower(std::string s) {
  for (char &c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return s;
}
inline bool utf8(const std::string &s) {
  for (size_t i=0; i<s.size();) {
    unsigned char a=s[i++]; if (a < 0x80) continue;
    int n=0; unsigned value=0, minimum=0;
    if (a>=0xc2 && a<=0xdf) { n=1; value=a&31; minimum=0x80; }
    else if (a>=0xe0 && a<=0xef) { n=2; value=a&15; minimum=0x800; }
    else if (a>=0xf0 && a<=0xf4) { n=3; value=a&7; minimum=0x10000; }
    else return false;
    if (i+n>s.size()) return false;
    while (n--) { unsigned char b=s[i++]; if ((b&0xc0)!=0x80) return false; value=(value<<6)|(b&63); }
    if (value<minimum || value>0x10ffff || (value>=0xd800 && value<=0xdfff)) return false;
  }
  return true;
}
inline FileSqlFingerprint fingerprint(const struct stat &s) {
  FileSqlFingerprint f; f.device=s.st_dev; f.inode=s.st_ino; f.size=s.st_size;
  f.mtime_sec=s.st_mtim.tv_sec; f.mtime_nsec=s.st_mtim.tv_nsec;
  f.ctime_sec=s.st_ctim.tv_sec; f.ctime_nsec=s.st_ctim.tv_nsec; return f;
}
inline bool equal(const FileSqlFingerprint &a, const FileSqlFingerprint &b) {
  return a.device==b.device && a.inode==b.inode && a.size==b.size &&
    a.mtime_sec==b.mtime_sec && a.mtime_nsec==b.mtime_nsec &&
    a.ctime_sec==b.ctime_sec && a.ctime_nsec==b.ctime_nsec;
}
inline bool under(const std::string &p,const std::string &root) {
  return root=="/" || (p.size()>root.size() && p.compare(0,root.size(),root)==0 && p[root.size()]=='/');
}
inline int open_root(const std::string &root) {
  int fd=::open(root[0]=='/'?"/":".",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
  for(size_t pos=0;fd>=0 && pos<root.size();) {
    size_t end=root.find('/',pos); if(end==std::string::npos) end=root.size();
    std::string part=root.substr(pos,end-pos); pos=end+1;
    if(part=="..") { ::close(fd); return -1; }
    if(part.empty() || part==".") continue;
    int next=::openat(fd,part.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    ::close(fd); fd=next;
  }
  return fd;
}
// Open components beneath the root, never follow links, never block on FIFOs.
inline int secure_open(const std::string &path, const std::string &root, bool directory,
                       std::string &canonical, std::string &why) {
  if (root.empty() || lower(root)=="null" || root.find('\0')!=std::string::npos ||
      path.empty() || path.find('\0')!=std::string::npos || path.size()>4096 || root.size()>4096) {
    why="secure_file_priv must be a nonempty valid directory and path must be valid"; return -1;
  }
  char *rp=::realpath(root.c_str(),nullptr);
  if (!rp) { why="cannot canonicalize secure_file_priv"; return -1; }
  std::string base(rp); std::free(rp);
  int fd=open_root(root);
  if (fd<0) { why="cannot open secure_file_priv directory (symlinks forbidden)"; return -1; }
  std::string rel=path;
  if (path[0]=='/') {
    if (path==base && directory) rel="";
    else if (!under(path,base)) { ::close(fd); why="path escapes secure_file_priv"; return -1; }
    else rel=path.substr(base=="/"?1:base.size()+1);
  }
  canonical=base;
  size_t pos=0; std::vector<std::string> parts;
  while (pos<rel.size()) {
    size_t end=rel.find('/',pos); if(end==std::string::npos) end=rel.size();
    std::string part=rel.substr(pos,end-pos); pos=end+1;
    if (part=="..") { ::close(fd); why="parent traversal is forbidden"; return -1; }
    if (!part.empty() && part!=".") parts.push_back(part);
  }
  if (parts.size()>256 || (!directory && parts.empty())) { ::close(fd); why="invalid path depth or file path"; return -1; }
  for (size_t i=0;i<parts.size();++i) {
    bool dir=directory || i+1<parts.size();
    int next=::openat(fd,parts[i].c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK|(dir?O_DIRECTORY:0));
    ::close(fd); fd=next;
    if (fd<0) { why="cannot open path component (missing, inaccessible or symlink)"; return -1; }
    if(canonical!="/") canonical+='/';
    canonical+=parts[i];
  }
  struct stat st;
  if (::fstat(fd,&st)!=0 || (directory?!S_ISDIR(st.st_mode):!S_ISREG(st.st_mode))) {
    ::close(fd); why="opened path is not a regular file/directory"; return -1;
  }
  char proc[64]; std::snprintf(proc,sizeof(proc),"/proc/self/fd/%d",fd);
  rp=::realpath(proc,nullptr);
  bool safe=rp && ((directory && std::string(rp)==base) || under(rp?rp:"",base));
  if(rp) std::free(rp);
  if (!safe) { ::close(fd); why="opened path escaped secure_file_priv"; return -1; }
  return fd;
}
inline bool digits(const std::string &s,size_t at,size_t n) {
  if (at+n>s.size()) return false;
  for(size_t i=at;i<at+n;++i) if(s[i]<'0'||s[i]>'9') return false;
  return true;
}
inline int number(const std::string &s,size_t at,size_t n) {
  int v=0; while(n--) v=v*10+s[at++]-'0'; return v;
}
inline bool date(const std::string &s) {
  if(s.size()<10 || s[4]!='-' || s[7]!='-' || !digits(s,0,4) || !digits(s,5,2) || !digits(s,8,2)) return false;
  int y=number(s,0,4),m=number(s,5,2),d=number(s,8,2);
  static const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};
  return y>=1 && m>=1 && m<=12 && d>=1 && d<=days[m-1]+(m==2 && (y%400==0 || (y%4==0 && y%100!=0)));
}
inline int classify(std::string &s,FileSqlType &t) {
  std::string l=lower(s);
  if(l=="true" || l=="false") { s=l; t=FileSqlType::BOOLEAN; return OB_SUCCESS; }
  if(l=="nan" || l=="inf" || l=="infinity" || l=="+inf" || l=="-inf" || l=="+infinity" || l=="-infinity") return OB_INVALID_DATA;
  if(s.size()>=10 && digits(s,0,4) && s[4]=='-' && s[7]=='-') {
    if(!date(s)) return OB_INVALID_DATA;
    if(s.size()==10) { t=FileSqlType::DATE; return OB_SUCCESS; }
    if(s.size()<19 || (s[10]!=' ' && s[10]!='T') || !digits(s,11,2) || s[13]!=':' ||
       !digits(s,14,2) || s[16]!=':' || !digits(s,17,2) || number(s,11,2)>23 ||
       number(s,14,2)>59 || number(s,17,2)>59) return OB_INVALID_DATA;
    if(s.size()>19 && (s[19]!='.' || s.size()<21 || s.size()>26 || !digits(s,20,s.size()-20))) return OB_INVALID_DATA;
    s[10]=' '; t=FileSqlType::DATETIME; return OB_SUCCESS;
  }
  size_t p=0; bool negative=false;
  if(p<s.size() && (s[p]=='+' || s[p]=='-')) { negative=s[p]=='-'; ++p; }
  size_t start=p; while(p<s.size() && s[p]>='0' && s[p]<='9') ++p;
  if(p>start && p==s.size()) {
    uint64_t v=0,limit=negative?uint64_t(INT64_MAX)+1:uint64_t(INT64_MAX);
    for(size_t i=start;i<s.size();++i) { unsigned d=s[i]-'0'; if(v>(limit-d)/10) return OB_INVALID_DATA; v=v*10+d; }
    t=FileSqlType::BIGINT; return OB_SUCCESS;
  }
  bool has_digits=p>start;
  if(p<s.size() && s[p]=='.') { size_t before=++p; while(p<s.size() && s[p]>='0' && s[p]<='9') ++p; has_digits=has_digits || p>before; }
  if(has_digits && p<s.size() && (s[p]=='e'||s[p]=='E')) {
    ++p; if(p<s.size() && (s[p]=='+'||s[p]=='-')) ++p;
    size_t before=p; while(p<s.size() && s[p]>='0' && s[p]<='9') ++p;
    if(p==before) has_digits=false;
  }
  if(has_digits && p==s.size()) {
    double v=0; std::istringstream in(s); in.imbue(std::locale::classic()); in>>v;
    if(in.fail() || !std::isfinite(v)) return OB_INVALID_DATA;
    t=FileSqlType::DOUBLE; return OB_SUCCESS;
  }
  t=FileSqlType::VARCHAR; return OB_SUCCESS;
}
inline FileSqlType merge_type(FileSqlType a,FileSqlType b) {
  if(a==FileSqlType::NULL_VALUE) return b;
  if(b==FileSqlType::NULL_VALUE || a==b) return a;
  if((a==FileSqlType::BIGINT && b==FileSqlType::DOUBLE)||(b==FileSqlType::BIGINT && a==FileSqlType::DOUBLE)) return FileSqlType::DOUBLE;
  if((a==FileSqlType::DATE && b==FileSqlType::DATETIME)||(b==FileSqlType::DATE && a==FileSqlType::DATETIME)) return FileSqlType::DATETIME;
  return FileSqlType::VARCHAR;
}
struct GlobState {
  struct Entry { std::string path; FileSqlFingerprint fp; };
  std::vector<Entry> files, directories;
  size_t position=0;
  int validate(const std::string &root,std::string &why,bool all_files) const {
    auto check=[&](const Entry &e,bool dir) -> bool {
      std::string canonical;
      int fd=secure_open(e.path,root,dir,canonical,why);
      if(fd<0) return false;
      struct stat st; bool same=::fstat(fd,&st)==0 && equal(fingerprint(st),e.fp); ::close(fd);
      if(!same) why="glob entry fingerprint changed: "+e.path;
      return same;
    };
    for(const auto &d:directories) if(!check(d,true)) return OB_INVALID_DATA;
    if(all_files) { for(const auto &e:files) if(!check(e,false)) return OB_INVALID_DATA; }
    else if(position<files.size() && !check(files[position],false)) return OB_INVALID_DATA;
    return OB_SUCCESS;
  }
  int discover(const std::string &pattern,const std::string &root,FileSqlFingerprint &out,std::string &why) {
    files.clear(); directories.clear(); position=0;
    if(pattern.empty() || pattern.size()>4096 || pattern.find('\0')!=std::string::npos || !utf8(pattern)) {
      why="invalid glob pattern"; return OB_INVALID_ARGUMENT;
    }
    char *rp=::realpath(root.c_str(),nullptr);
    if(!rp) { why="invalid secure_file_priv directory"; return OB_INVALID_ARGUMENT; }
    std::string base(rp); std::free(rp);
    std::string canonical;
    int root_fd=secure_open(base,root,true,canonical,why);
    if(root_fd<0) return OB_INVALID_ARGUMENT;
    struct stat root_stat; int stat_ret=::fstat(root_fd,&root_stat); ::close(root_fd);
    if(stat_ret!=0) { why="cannot stat glob root"; return OB_IO_ERROR; }
    std::string rel=pattern;
    if(pattern[0]=='/') {
      if(!under(pattern,base)) { why="glob pattern escapes secure_file_priv"; return OB_INVALID_ARGUMENT; }
      rel=pattern.substr(base=="/"?1:base.size()+1);
    }
    std::vector<std::string> parts;
    for(size_t pos=0;pos<rel.size();) {
      size_t end=rel.find('/',pos); if(end==std::string::npos) end=rel.size();
      std::string part=rel.substr(pos,end-pos); pos=end+1;
      if(part=="..") { why="parent traversal is forbidden"; return OB_INVALID_ARGUMENT; }
      if(!part.empty() && part!=".") parts.push_back(part);
    }
    if(parts.empty() || parts.size()>64) { why="glob path depth limit exceeded"; return OB_INVALID_ARGUMENT; }
    size_t memory=0;
    std::map<std::string,FileSqlFingerprint> seen_dirs,seen_files;
    auto remember=[&](std::map<std::string,FileSqlFingerprint> &seen,const std::string &p,const struct stat &st) -> int {
      auto it=seen.find(p);
      if(it!=seen.end()) {
        if(!equal(it->second,fingerprint(st))) { why="glob directory changed during discovery"; return OB_INVALID_DATA; }
        return OB_SUCCESS;
      }
      if(seen_dirs.size()+seen_files.size()>=65536 || memory+p.size()>MAX_METADATA) {
        why="glob metadata/entry limit exceeded"; return OB_SIZE_OVERFLOW;
      }
      memory+=p.size(); seen.emplace(p,fingerprint(st)); return OB_SUCCESS;
    };
    std::function<int(const std::string&,size_t,size_t)> walk;
    walk=[&](const std::string &dir,size_t part,size_t depth) -> int {
      if(depth>64) { why="recursive glob depth limit exceeded"; return OB_SIZE_OVERFLOW; }
      std::string canonical_dir;
      int fd=secure_open(dir,root,true,canonical_dir,why); if(fd<0) return OB_INVALID_ARGUMENT;
      struct stat before;
      if(::fstat(fd,&before)!=0) { ::close(fd); why="cannot stat glob directory"; return OB_IO_ERROR; }
      int ret=remember(seen_dirs,canonical_dir,before); if(ret) { ::close(fd); return ret; }
      DIR *raw=::fdopendir(fd); if(!raw) { ::close(fd); why="cannot read glob directory"; return OB_IO_ERROR; }
      std::unique_ptr<DIR,int(*)(DIR*)> stream(raw,::closedir);
      if(part==parts.size()) return OB_SUCCESS;
      bool recursive=parts[part]=="**";
      if(recursive && part+1<parts.size()) { ret=walk(dir,part+1,depth); if(ret) return ret; }
      while(true) {
        errno=0; struct dirent *e=::readdir(raw);
        if(!e) { if(errno!=0) { why="glob directory read error"; return OB_IO_ERROR; } break; }
        std::string name(e->d_name);
        if(name=="." || name==".." || (recursive && !name.empty() && name[0]=='.')) continue;
        if(!recursive && ::fnmatch(parts[part].c_str(),name.c_str(),FNM_PERIOD)!=0) continue;
        if(!utf8(name)) { why="invalid UTF-8 glob filename"; return OB_INVALID_DATA; }
        std::string child=dir+(dir=="/"?"":"/")+name;
        if(child.size()>4096) { why="glob path size limit exceeded"; return OB_SIZE_OVERFLOW; }
        struct stat st;
        if(::fstatat(fd,name.c_str(),&st,AT_SYMLINK_NOFOLLOW)!=0) { why="glob entry changed during discovery"; return OB_IO_ERROR; }
        if(S_ISLNK(st.st_mode)) { why="glob symlink is forbidden"; return OB_INVALID_ARGUMENT; }
        if(S_ISDIR(st.st_mode)) {
          if(recursive || part+1<parts.size()) { ret=walk(child,recursive?part:part+1,depth+1); if(ret) return ret; }
        } else if(S_ISREG(st.st_mode) && (part+1==parts.size())) {
          ret=remember(seen_files,child,st); if(ret) return ret;
        } else if(!S_ISREG(st.st_mode)) { why="glob matched non-regular file"; return OB_INVALID_ARGUMENT; }
      }
      struct stat after;
      if(::fstat(fd,&after)!=0 || !equal(fingerprint(before),fingerprint(after))) { why="glob directory changed during discovery"; return OB_INVALID_DATA; }
      return OB_SUCCESS;
    };
    int ret=walk(base,0,0); if(ret) return ret;
    for(const auto &p:seen_dirs) directories.push_back({p.first,p.second});
    for(const auto &p:seen_files) files.push_back({p.first,p.second});
    if((ret=validate(root,why,true))!=OB_SUCCESS) return ret;
    // The interface fingerprint is opaque: glob hashes sorted names and every
    // constituent fingerprint, not file contents or directory enumeration order.
    uint64_t hash=1469598103934665603ULL;
    auto add=[&](uint64_t v) { for(int i=0;i<8;++i) { hash^=v&255; hash*=1099511628211ULL; v>>=8; } };
    auto hash_entry=[&](const Entry &e) {
      add(e.path.size()); for(unsigned char c:e.path) { hash^=c; hash*=1099511628211ULL; }
      add(e.fp.device); add(e.fp.inode); add(e.fp.size); add(e.fp.mtime_sec); add(e.fp.mtime_nsec); add(e.fp.ctime_sec); add(e.fp.ctime_nsec);
    };
    for(const auto &e:directories) hash_entry(e);
    for(const auto &e:files) hash_entry(e);
    out=fingerprint(root_stat); out.inode=hash; out.size=files.size(); return OB_SUCCESS;
  }
};
} } }
