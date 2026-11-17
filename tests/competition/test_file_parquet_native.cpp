// Standalone harness compiles the COMPLETE production file_sql_parquet.cpp.
#include "sql/engine/basic/file_sql_parquet.h"
#include "sql/engine/basic/file_sql_reader.h"
#include "lib/ob_errno.h"
#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace oceanbase::sql;
using namespace oceanbase::common;
static void require(bool ok,const std::string &msg) { if(!ok) throw std::runtime_error(msg); }
static std::string unhex(const std::string &s) {
  std::string r; for(size_t i=0;i<s.size();i+=2)r.push_back(static_cast<char>(std::stoul(s.substr(i,2),nullptr,16))); return r;
}
int main(int argc,char **argv) {
  try {
    require(argc>=3,"usage: test parquet expected-or-reject [projection]");
    int flags=O_RDONLY;
#ifdef _WIN32
    flags|=O_BINARY;
#endif
    int fd=::open(argv[1],flags); require(fd>=0,"open fixture");
    FileSqlParquetReader reader; FileSqlSchema schema;
    int ret=reader.infer(fd,schema);
    if(std::string(argv[2])=="reject") {
      if(ret==0) {
        ret=reader.open(fd,schema,{}); FileSqlRow row;
        while(ret==0) ret=reader.next(row);
      }
      require(ret!=0&&ret!=OB_ITER_END,"malformed/unsupported input accepted");
      require(!reader.error().empty(),"missing error diagnostic");
      reader.close(); require(lseek(fd,0,SEEK_SET)>=0,"borrowed fd closed"); ::close(fd);
      std::cout<<"PASS reject "<<argv[1]<<"\n"; return 0;
    }
    require(ret==0,"infer: "+reader.error()+" code="+std::to_string(ret));
    std::ifstream f(argv[2]); size_t nc,nr; f>>nc>>nr;
    require(schema.columns.size()==nc&&schema.row_count==static_cast<int64_t>(nr),"schema size/count");
    for(size_t i=0;i<nc;++i){std::string name; int type,nullable; f>>name>>type>>nullable;
      require(schema.columns[i].name==unhex(name)&&static_cast<int>(schema.columns[i].type)==type&&schema.columns[i].nullable==bool(nullable),"schema column "+std::to_string(i));}
    std::vector<FileSqlRow> expected(nr);
    for(auto &r:expected){r.cells.resize(nc); for(auto &c:r.cells){std::string s; f>>s;require(!s.empty(),"missing expected cell"); c.is_null=s=="N"; if(!c.is_null)c.text=unhex(s.substr(1));}}
    std::vector<int64_t> projection;
    if(argc>3){std::istringstream p(argv[3]);int64_t n;while(p>>n)projection.push_back(n);}
    require(reader.open(fd,schema,projection)==0,"open: "+reader.error());
    for(int pass=0;pass<2;++pass){
      for(size_t i=0;i<nr;++i){FileSqlRow row;int next_ret=reader.next(row);require(next_ret==0,"next row "+std::to_string(i)+": "+reader.error());require(row.cells.size()==nc,"row width");
        for(size_t j=0;j<nc;++j){bool selected=projection.empty();for(auto p:projection)if(p==static_cast<int64_t>(j))selected=true;
          if(selected)require(row.cells[j].is_null==expected[i].cells[j].is_null&&(row.cells[j].is_null||row.cells[j].text==expected[i].cells[j].text),"cell "+std::to_string(i)+","+std::to_string(j)+" got="+row.cells[j].text+" expected="+expected[i].cells[j].text);
          else require(row.cells[j].is_null,"unprojected cell materialized");}}
      FileSqlRow end; require(reader.next(end)==OB_ITER_END,"EOF");require(reader.next(end)==OB_ITER_END,"stable EOF");
      require(reader.rescan()==0,"rescan");
    }
    reader.close(); require(lseek(fd,0,SEEK_SET)>=0,"borrowed fd closed");::close(fd);
    std::cout<<"PASS rows="<<nr<<" columns="<<nc<<" "<<argv[1]<<"\n";
  }catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}return 0;
}
