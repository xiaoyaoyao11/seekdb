#pragma once
#include "sql/engine/basic/file_sql_plan.h"
#include "sql/engine/basic/file_sql_reader_impl.h"
#if defined(__linux__)
#include <sys/vfs.h>
#include <linux/magic.h>
#endif

namespace oceanbase { namespace sql {
inline bool file_sql_is_copy(const common::ObString &s) {
  return s.length()>=5 && std::memcmp(s.ptr(),"FSQL1",5)==0;
}
// Directory-fd anchored, no-overwrite publication. A failed or cancelled query
// leaves neither a partial target nor a temporary file.
class FileSqlCopyWriter {
public:
  FileSqlPlan plan;
  ~FileSqlCopyWriter(){close();}
  int open(const common::ObString &descriptor,const std::string &root){
    using namespace common;
    int ret=plan.decode(descriptor);if(ret) return ret;
    const size_t slash=plan.path.find_last_of('/');
    std::string parent=slash==std::string::npos?".":slash==0?"/":plan.path.substr(0,slash);
    target_=slash==std::string::npos?plan.path:plan.path.substr(slash+1);
    if(target_.empty()||target_=="."||target_=="..")return OB_INVALID_ARGUMENT;
    std::string canonical,why;
    directory_=file_sql_detail::secure_open(parent,root,true,canonical,why);
    if(directory_<0)return OB_INVALID_ARGUMENT;
    struct stat st;
    if(::fstatat(directory_,target_.c_str(),&st,AT_SYMLINK_NOFOLLOW)==0)return OB_FILE_ALREADY_EXIST;
    if(errno!=ENOENT)return OB_IO_ERROR;
    // An anonymous inode is stronger than a named file in a private staging
    // directory: there is no staging pathname to collide with, replace, or
    // accidentally unlink. Never fall back to pathname-based publication.
#if defined(__linux__) && defined(O_TMPFILE)
    file_=::openat(directory_,".",O_WRONLY|O_TMPFILE|O_CLOEXEC,0600);
    if(file_<0)return errno==EOPNOTSUPP||errno==EINVAL||errno==EISDIR||errno==ENOSYS
        ?OB_NOT_SUPPORTED:OB_IO_ERROR;
    // Anchor the kernel's own fd directory, not a later /proc pathname lookup.
    // This route needs no CAP_DAC_READ_SEARCH (unlike AT_EMPTY_PATH).
    descriptors_=::open("/proc/self/fd",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    struct statfs proc;
    if(descriptors_<0||::fstatfs(descriptors_,&proc)!=0||proc.f_type!=PROC_SUPER_MAGIC)
      return OB_NOT_SUPPORTED;
#else
    return OB_NOT_SUPPORTED;
#endif
    parent_=parent;root_=root;
    if(plan.options.header){
      FileSqlRow row;for(const auto &c:plan.schema.columns)row.cells.push_back({false,c.name});
      ret=write(row);
    }
    return ret;
  }
  int write(const FileSqlRow &row){
    using namespace common;
    if(file_<0)return OB_NOT_INIT;
    if(row.cells.size()!=plan.schema.columns.size())return OB_INVALID_ARGUMENT;
    std::string out;
    for(size_t i=0;i<row.cells.size();++i){
      if(i)out+=plan.options.delimiter;
      const auto &c=row.cells[i];
      if(c.is_null)out+=plan.options.nullstr;
      else{
        if(c.text.size()>file_sql_detail::MAX_RECORD || !file_sql_detail::utf8(c.text) || c.text.find('\0')!=std::string::npos)return OB_INVALID_DATA;
        bool quote=c.text.empty()||c.text==plan.options.nullstr||c.text.find_first_of(std::string()+plan.options.delimiter+plan.options.quote+"\r\n")!=std::string::npos;
        if(quote)out+=plan.options.quote;
        for(char v:c.text){if(quote&&(v==plan.options.quote||v==plan.options.escape))out+=plan.options.escape;out+=v;}
        if(quote)out+=plan.options.quote;
      }
      if(out.size()>file_sql_detail::MAX_RECORD)return OB_SIZE_OVERFLOW;
    }
    out+='\n';size_t done=0;
    if(written_+out.size()>uint64_t(1024)*1024*1024)return OB_SIZE_OVERFLOW;
    while(done<out.size()){
      ssize_t n=::write(file_,out.data()+done,out.size()-done);
      if(n<0&&errno==EINTR)continue;
      if(n<=0)return OB_IO_ERROR;done+=n;
    }
    written_+=out.size();return OB_SUCCESS;
  }
  int finish(){
    using namespace common;
    if(file_<0)return OB_NOT_INIT;
    struct stat old,now;std::string canonical,why;
    int verify=file_sql_detail::secure_open(parent_,root_,true,canonical,why);
    bool same=verify>=0&&::fstat(directory_,&old)==0&&::fstat(verify,&now)==0&&old.st_dev==now.st_dev&&old.st_ino==now.st_ino;
    if(verify>=0)::close(verify);
    if(!same)return OB_INVALID_DATA;
    if(::fsync(file_)!=0)return OB_IO_ERROR;
    // Keep the fd alive through the atomic no-replace commit. The numeric
    // entry in our anchored procfs fd directory denotes this exact inode,
    // even if a local actor swaps arbitrary names in the output directory.
    const std::string source=std::to_string(file_);
    if(::linkat(descriptors_,source.c_str(),directory_,target_.c_str(),AT_SYMLINK_FOLLOW)!=0)
      return errno==EEXIST?OB_FILE_ALREADY_EXIST:OB_IO_ERROR;
    // Publication is the commit point. Never attempt pathname rollback: an
    // actor could already have replaced the target with an unowned inode.
    close();return OB_SUCCESS;
  }
  void close(){
    if(file_>=0){::close(file_);file_=-1;}
    if(descriptors_>=0){::close(descriptors_);descriptors_=-1;}
    if(directory_>=0){::close(directory_);directory_=-1;}
  }
private:
  int directory_=-1,file_=-1,descriptors_=-1;
  uint64_t written_=0;
  std::string target_,parent_,root_;
};
} }
