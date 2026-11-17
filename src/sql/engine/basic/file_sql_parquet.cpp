#include "sql/engine/basic/file_sql_parquet.h"
#include "sql/engine/basic/file_sql_reader.h"
#include "lib/ob_errno.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace oceanbase { namespace sql {
namespace {
using namespace oceanbase::common;
constexpr size_t MAX_FOOTER=16U<<20, MAX_PAGE=16U<<20, MAX_HEADER=64U<<10;
constexpr size_t MAX_VALUES=1U<<20, MAX_COLUMNS=256, MAX_GROUPS=65536;
constexpr size_t MAX_CELL=8U<<20, MAX_MEMORY=128U<<20;
struct Failure { int code; std::string text; };
void check(bool ok,const char *why){if(!ok)throw Failure{OB_INVALID_ARGUMENT,why};}
void supported(bool ok,const char *why){if(!ok)throw Failure{OB_NOT_SUPPORTED,why};}
struct Bytes {
  const uint8_t *p;size_t n,pos=0;
  Bytes(const uint8_t *p_,size_t n_):p(p_),n(n_){}
  explicit Bytes(const std::vector<uint8_t>&v):Bytes(v.data(),v.size()){}
  size_t left()const{return n-pos;}
  uint8_t byte(){check(pos<n,"truncated Parquet stream");return p[pos++];}
  uint64_t le(unsigned count){check(count<=8&&count<=left(),"truncated fixed value");uint64_t x=0;for(unsigned i=0;i<count;++i)x|=uint64_t(byte())<<(8*i);return x;}
  uint64_t var(){uint64_t x=0;for(unsigned i=0;i<10;++i){uint8_t b=byte();check(i<9||b<=1,"varint overflow");x|=uint64_t(b&127)<<(7*i);if(!(b&128))return x;}throw Failure{OB_INVALID_ARGUMENT,"varint overflow"};}
  int64_t zig(){uint64_t x=var();return static_cast<int64_t>(x>>1)^-static_cast<int64_t>(x&1);}
  void skip(size_t k){check(k<=left(),"truncated bytes");pos+=k;}
};
// Compact Thrift: bounded parsing of arbitrary fields, including unknowns.
struct Node {
  int id=0,kind=0;int64_t value=0;std::string text;std::vector<Node> children;
  const Node *find(int key)const{for(const auto &c:children)if(c.id==key)return &c;return nullptr;}
};
struct Compact {
  Bytes &b;size_t nodes=0;
  explicit Compact(Bytes &x):b(x){}
  Node value(int type,unsigned depth=0,bool collection=false){
    check(depth<=24&&++nodes<=200000,"Compact Thrift complexity limit");Node n;n.kind=type;
    if(type==1||type==2)n.value=collection?(b.byte()==1):type==1;
    else if(type==3)n.value=static_cast<int8_t>(b.byte());
    else if(type>=4&&type<=6)n.value=b.zig();
    else if(type==7)n.value=static_cast<int64_t>(b.le(8));
    else if(type==8){uint64_t len=b.var();check(len<=MAX_FOOTER&&len<=b.left(),"Compact binary length");n.text.assign(reinterpret_cast<const char*>(b.p+b.pos),static_cast<size_t>(len));b.skip(static_cast<size_t>(len));}
    else if(type==12){int last=0;for(;;){uint8_t h=b.byte();if(h==0)break;int t=h&15;check(t>=1&&t<=12,"Compact field type");int64_t id=(h>>4)?last+(h>>4):b.zig();check(id>0&&id<=32767,"Compact field id");check(!n.find(static_cast<int>(id)),"duplicate Compact field");Node child=value(t,depth+1);child.id=static_cast<int>(id);last=child.id;n.children.push_back(std::move(child));}}
    else if(type==9||type==10){uint8_t h=b.byte();uint64_t len=h>>4;if(len==15)len=b.var();int t=h&15;check(len<=200000&&len<=b.left()+1&&t>=1&&t<=12,"Compact list limit");for(uint64_t i=0;i<len;++i)n.children.push_back(value(t,depth+1,true));}
    else if(type==11){uint64_t len=b.var();check(len<=100000&&len<=b.left(),"Compact map limit");if(len){uint8_t h=b.byte();for(uint64_t i=0;i<len;++i){n.children.push_back(value(h>>4,depth+1,true));n.children.push_back(value(h&15,depth+1,true));}}}
    else check(false,"Compact type unsupported");
    return n;
  }
};
const Node &field(const Node &n,int id,int type){const Node *p=n.find(id);check(p&&p->kind==type,"missing/wrong Compact field");return *p;}
int64_t number(const Node &n,int id,int type=5){return field(n,id,type).value;}
int64_t optional(const Node &n,int id,int64_t def,int type=5){const Node *p=n.find(id);if(!p)return def;check(p->kind==type,"wrong Compact numeric type");return p->value;}
void read_at(int fd,uint64_t offset,uint8_t *data,size_t size){
  check(offset<=static_cast<uint64_t>(INT64_MAX)&&size<=MAX_FOOTER,"Parquet read range");size_t done=0;
#ifdef _WIN32
  // CRT has no pread: native Windows focused tests preserve borrowed position.
  const auto saved=_lseeki64(fd,0,SEEK_CUR);
  if(saved<0||_lseeki64(fd,static_cast<int64_t>(offset),SEEK_SET)<0)throw Failure{OB_IO_ERROR,"Parquet seek failed"};
#endif
  while(done<size){
#ifdef _WIN32
    int n=_read(fd,data+done,static_cast<unsigned>(size-done));
#else
    ssize_t n=::pread(fd,data+done,size-done,static_cast<off_t>(offset+done));
#endif
    if(n<0&&errno==EINTR)continue;
    if(n<=0){
#ifdef _WIN32
      _lseeki64(fd,saved,SEEK_SET);
#endif
      throw Failure{OB_IO_ERROR,"short Parquet read"};
    }done+=static_cast<size_t>(n);
  }
#ifdef _WIN32
  if(_lseeki64(fd,saved,SEEK_SET)<0)throw Failure{OB_IO_ERROR,"Parquet restore seek failed"};
#endif
}
std::vector<uint8_t> read_at(int fd,uint64_t off,size_t len){check(len<=MAX_PAGE,"Parquet read size limit");std::vector<uint8_t> out(len);read_at(fd,off,out.data(),len);return out;}
uint64_t file_size(int fd){
#ifdef _WIN32
  struct _stat64 s;check(_fstat64(fd,&s)==0&&(s.st_mode&_S_IFREG)!=0,"Parquet descriptor must be regular");
#else
  struct stat s;check(fstat(fd,&s)==0&&S_ISREG(s.st_mode),"Parquet descriptor must be regular");
#endif
  check(s.st_size>=12,"Parquet file too short");return static_cast<uint64_t>(s.st_size);
}
struct Column {FileSqlColumn sql;int physical=-1,converted=-1,unit=0,integer_bits=0;bool integer_signed=true;};
struct Chunk {int physical=-1,codec=-1;int64_t count=-1;uint64_t begin=0,end=0,data=0;};
struct Group {int64_t rows=0;std::vector<Chunk> chunks;};
struct Metadata {FileSqlSchema schema;std::vector<Column> columns;std::vector<Group> groups;};
Metadata metadata(int fd){
  uint64_t size=file_size(fd);auto magic=read_at(fd,0,4);auto tail=read_at(fd,size-8,8);
  check(!std::memcmp(magic.data(),"PAR1",4)&&!std::memcmp(tail.data()+4,"PAR1",4),"invalid/encrypted Parquet magic");
  Bytes end(tail);uint64_t len=end.le(4);check(len>0&&len<=MAX_FOOTER&&len<=size-12,"Parquet footer length limit");uint64_t footer=size-8-len;
  auto raw=read_at(fd,footer,static_cast<size_t>(len));Bytes bytes(raw);Compact compact(bytes);Node root=compact.value(12);check(bytes.left()==0,"trailing footer bytes");
  supported(!root.find(8)&&!root.find(9),"encrypted Parquet unsupported");check(number(root,1)>0,"invalid Parquet version");Metadata m;m.schema.row_count=number(root,3,6);check(m.schema.row_count>=0,"negative row count");
  const auto &schema=field(root,2,9).children;check(schema.size()>=2&&schema.size()<=MAX_COLUMNS+1,"Parquet column limit");check(schema[0].kind==12&&!schema[0].find(1),"invalid schema root");supported(number(schema[0],5)==static_cast<int64_t>(schema.size()-1),"nested Parquet schema unsupported");
  for(size_t i=1;i<schema.size();++i){const Node &s=schema[i];check(s.kind==12,"schema element type");Column c;c.physical=static_cast<int>(number(s,1));int64_t rep=number(s,3);supported(rep==0||rep==1,"repeated Parquet unsupported");supported(!s.find(5),"nested Parquet fields unsupported");c.sql.name=field(s,4,8).text;check(!c.sql.name.empty()&&c.sql.name.size()<=4096&&c.sql.name.find('\0')==std::string::npos,"invalid column name");for(const auto &prior:m.columns)check(prior.sql.name!=c.sql.name,"duplicate column name");c.sql.nullable=rep==1;c.converted=static_cast<int>(optional(s,6,-1));
    supported(c.physical==0||c.physical==1||c.physical==2||c.physical==4||c.physical==5||c.physical==6,"unsupported Parquet primitive type");
    c.sql.type=c.physical==0?FileSqlType::BOOLEAN:(c.physical==1||c.physical==2)?FileSqlType::BIGINT:(c.physical==4||c.physical==5)?FileSqlType::DOUBLE:FileSqlType::VARCHAR;
    if(c.converted==6){supported(c.physical==1,"DATE must use INT32");c.sql.type=FileSqlType::DATE;}
    else if(c.converted==9||c.converted==10){supported(c.physical==2,"TIMESTAMP must use INT64");c.sql.type=FileSqlType::DATETIME;c.unit=c.converted==9?1:2;}
    else if(c.converted>=11&&c.converted<=18){
      c.integer_signed=c.converted>=15;
      c.integer_bits=8 << (c.converted-(c.integer_signed?15:11));
      supported(c.physical==(c.integer_bits==64?2:1),"INTEGER physical mismatch");
    }
    else supported(c.converted==-1||(c.converted==0&&c.physical==6),"unsupported Parquet converted type");
    if(const Node *logical=s.find(10)){
      check(logical->kind==12&&logical->children.size()==1,"logical type union");const Node &lt=logical->children[0];check(lt.kind==12,"logical type body");
      if(lt.id==1){supported(c.physical==6&&(c.converted==-1||c.converted==0),"STRING physical/converted mismatch");}
      else if(lt.id==6){supported(c.physical==1&&(c.converted==-1||c.converted==6),"DATE physical/converted mismatch");c.sql.type=FileSqlType::DATE;}
      else if(lt.id==8){supported(c.physical==2,"TIMESTAMP must use INT64");check(lt.find(1)!=nullptr,"missing TIMESTAMP adjusted flag");const Node &adjusted=*lt.find(1);check(adjusted.kind==1||adjusted.kind==2,"TIMESTAMP adjusted boolean");const Node &unit=field(lt,2,12);check(unit.children.size()==1&&unit.children[0].kind==12,"TIMESTAMP time unit union");int u=unit.children[0].id;supported(u>=1&&u<=3,"TIMESTAMP unit unsupported");supported(c.converted==-1||(c.converted==9&&u==1)||(c.converted==10&&u==2),"TIMESTAMP converted mismatch");c.unit=u;c.sql.type=FileSqlType::DATETIME;}
      else if(lt.id==10){
        int bits=static_cast<int>(number(lt,1,3));
        const Node *sign=lt.find(2);check(sign&&(sign->kind==1||sign->kind==2),"INTEGER signed flag");
        supported(bits==8||bits==16||bits==32||bits==64,"INTEGER width");
        supported(c.physical==(bits==64?2:1),"INTEGER physical mismatch");
        supported(c.converted==-1||(c.integer_bits==bits&&c.integer_signed==(sign->value!=0)),"INTEGER converted mismatch");
        c.integer_bits=bits;c.integer_signed=sign->value!=0;
      }
      else supported(false,"unsupported Parquet logical type");
    }
    if(c.sql.type==FileSqlType::DATETIME)c.sql.scale=c.unit==1?3:6;
    m.schema.columns.push_back(c.sql);m.columns.push_back(std::move(c));
  }
  const auto &groups=field(root,4,9).children;check(groups.size()<=MAX_GROUPS,"rowgroup limit");int64_t total=0;
  for(const auto &g:groups){check(g.kind==12,"rowgroup type");Group group;group.rows=number(g,3,6);check(group.rows>=0&&group.rows<=INT64_MAX-total,"rowgroup row count");total+=group.rows;const auto &chunks=field(g,1,9).children;check(chunks.size()==m.columns.size(),"rowgroup column count");
    for(size_t i=0;i<chunks.size();++i){const Node &cc=chunks[i];check(cc.kind==12,"column chunk type");supported(!cc.find(1),"external column files unsupported");supported(!cc.find(8)&&!cc.find(9),"encrypted column unsupported");const Node &cm=field(cc,3,12);Chunk chunk;chunk.physical=static_cast<int>(number(cm,1));chunk.codec=static_cast<int>(number(cm,4));chunk.count=number(cm,5,6);check(chunk.count==group.rows,"flat column count mismatch");check(chunk.physical==m.columns[i].physical,"column physical type mismatch");supported(chunk.codec==0||chunk.codec==1,"unsupported codec (only uncompressed/Snappy)");const auto &path=field(cm,3,9).children;check(path.size()==1&&path[0].kind==8&&path[0].text==m.columns[i].sql.name,"column path mismatch");int64_t data=number(cm,9,6),dict=optional(cm,11,data,6),compressed=number(cm,7,6);check(data>=4&&dict>=4&&dict<=data&&compressed>=0,"invalid column offset");chunk.data=static_cast<uint64_t>(data);chunk.begin=static_cast<uint64_t>(dict);check(chunk.begin<=footer&&static_cast<uint64_t>(compressed)<=footer-chunk.begin,"column chunk exceeds data region");chunk.end=chunk.begin+static_cast<uint64_t>(compressed);check(chunk.data<=chunk.end,"data offset outside chunk");
      const auto &encodings=field(cm,2,9).children;for(const auto &e:encodings){check(e.kind==5,"encoding enum type");supported(e.value==0||e.value==2||e.value==3||e.value==8,"Parquet encoding unsupported");}group.chunks.push_back(std::move(chunk));
    }m.groups.push_back(std::move(group));
  }check(total==m.schema.row_count,"footer row count mismatch");return m;
}
// Raw Snappy per google/snappy format_description.txt. Overlapping copies legal.
std::vector<uint8_t> decompress(const std::vector<uint8_t>&input,size_t expected,int codec){
  check(expected<=MAX_PAGE,"uncompressed page size limit");if(codec==0){check(input.size()==expected,"uncompressed page size mismatch");return input;}supported(codec==1,"unsupported compression codec");Bytes b(input);check(b.var()==expected,"Snappy output length mismatch");std::vector<uint8_t> out;out.reserve(expected);
  while(b.left()){uint8_t tag=b.byte();unsigned type=tag&3;uint64_t count=0,offset=0;if(type==0){unsigned k=tag>>2;count=k<60?k+1:b.le(k-59)+1;check(count<=b.left()&&count<=expected-out.size(),"Snappy literal bounds");out.insert(out.end(),b.p+b.pos,b.p+b.pos+static_cast<size_t>(count));b.skip(static_cast<size_t>(count));}else{if(type==1){count=4+((tag>>2)&7);offset=((tag&224)<<3)|b.byte();}else{count=1+(tag>>2);offset=b.le(type==2?2:4);}check(offset>0&&offset<=out.size()&&count<=expected-out.size(),"Snappy copy bounds");for(uint64_t i=0;i<count;++i)out.push_back(out[out.size()-static_cast<size_t>(offset)]);}}
  check(out.size()==expected,"Snappy truncated output");return out;
}
std::vector<uint32_t> hybrid(Bytes &b,unsigned width,size_t count){
  check(width<=32&&count<=MAX_VALUES,"RLE/bitpack limits");std::vector<uint32_t> out;out.reserve(count);
  while(out.size()<count){uint64_t header=b.var();check(header!=0,"empty RLE run");uint64_t run=header>>1;
    if(!(header&1)){check(run>0&&run<=count-out.size(),"RLE run exceeds value count");uint64_t v=b.le((width+7)/8);check(width==32||v<(uint64_t(1)<<width),"RLE value width");out.insert(out.end(),static_cast<size_t>(run),static_cast<uint32_t>(v));}
    else{check(run>0&&run<=(MAX_VALUES+7)/8,"bitpack run limit");uint64_t values=run*8;check(values<=count-out.size()+7,"bitpack run exceeds count");size_t bytes=static_cast<size_t>(run)*width;check(bytes<=b.left(),"truncated bitpack run");size_t take=std::min(static_cast<size_t>(values),count-out.size());for(size_t i=0;i<take;++i){uint32_t v=0;for(unsigned bit=0;bit<width;++bit){size_t pos=i*width+bit;v|=uint32_t((b.p[b.pos+pos/8]>>(pos%8))&1)<<bit;}out.push_back(v);}b.skip(bytes);}
  }return out;
}
struct Cursor {
  Chunk chunk;uint64_t offset=0;size_t page_row=0,page_rows=0,values_pos=0,bool_bit=0;int encoding=0;int64_t loaded=0;bool dictionary_seen=false;
  std::vector<uint8_t> values;std::vector<uint32_t> definitions,indices;std::vector<std::string> dictionary;
  size_t memory()const{size_t n=values.capacity()+definitions.capacity()*sizeof(uint32_t)+indices.capacity()*sizeof(uint32_t)+dictionary.capacity()*sizeof(std::string);for(const auto &s:dictionary)n+=s.capacity();return n;}
};
uint32_t crc32(const std::vector<uint8_t>&v){uint32_t c=~uint32_t(0);for(uint8_t b:v){c^=b;for(int k=0;k<8;++k)c=(c>>1)^(0xedb88320U&-(c&1));}return ~c;}
bool utf8(const std::string &s){
  size_t i=0;while(i<s.size()){uint8_t a=static_cast<uint8_t>(s[i++]);if(a<128)continue;unsigned n=0;uint32_t v=0,min=0;
    if(a>=0xc2&&a<=0xdf){n=1;v=a&31;min=0x80;}else if(a>=0xe0&&a<=0xef){n=2;v=a&15;min=0x800;}else if(a>=0xf0&&a<=0xf4){n=3;v=a&7;min=0x10000;}else return false;
    if(n>s.size()-i){return false;}
    for(unsigned j=0;j<n;++j){uint8_t b=static_cast<uint8_t>(s[i++]);if((b&0xc0)!=0x80)return false;v=(v<<6)|(b&63);}if(v<min||v>0x10ffff||(v>=0xd800&&v<=0xdfff))return false;
  }return true;
}
// Proleptic Gregorian civil date from Unix days; no timezone or libc range dependency.
std::string date_text(int64_t days){
  int64_t z=days+719468,era=(z>=0?z:z-146096)/146097;unsigned doe=static_cast<unsigned>(z-era*146097);unsigned yoe=(doe-doe/1460+doe/36524-doe/146096)/365;int64_t y=static_cast<int64_t>(yoe)+era*400;unsigned doy=doe-(365*yoe+yoe/4-yoe/100);unsigned mp=(5*doy+2)/153;unsigned d=doy-(153*mp+2)/5+1;int month=static_cast<int>(mp)+(mp<10?3:-9);y+=month<=2;supported(y>=1&&y<=9999,"Parquet date outside SQL year 1..9999");std::ostringstream out;out.imbue(std::locale::classic());out<<std::setfill('0')<<std::setw(4)<<y<<'-'<<std::setw(2)<<month<<'-'<<std::setw(2)<<d;return out.str();
}
std::string timestamp_text(int64_t raw,int unit){
  int64_t per_second=unit==1?1000:unit==2?1000000:1000000000;int64_t per_day=86400*per_second;int64_t day=raw/per_day,remainder=raw%per_day;if(remainder<0){--day;remainder+=per_day;}int64_t seconds=remainder/per_second,sub=remainder%per_second;
  supported(unit!=3||sub%1000==0,"submicrosecond Parquet timestamps unsupported");int64_t micros=unit==1?sub*1000:unit==2?sub:sub/1000;std::ostringstream out;out.imbue(std::locale::classic());out<<date_text(day)<<' '<<std::setfill('0')<<std::setw(2)<<seconds/3600<<':'<<std::setw(2)<<(seconds/60)%60<<':'<<std::setw(2)<<seconds%60;if(micros)out<<'.'<<std::setw(6)<<micros;return out.str();
}
std::string plain(Bytes &b,const Column &column,size_t &bool_bit){
  switch(column.physical){
    case 0:{check(b.left()>0,"truncated boolean value");bool v=(b.p[b.pos]>>bool_bit)&1;if(++bool_bit==8){++b.pos;bool_bit=0;}return v?"true":"false";}
    case 1:{uint32_t v=static_cast<uint32_t>(b.le(4));int32_t n;std::memcpy(&n,&v,4);
      if(column.integer_bits){
        if(!column.integer_signed){check(column.integer_bits==32||uint64_t(v)<(uint64_t(1)<<column.integer_bits),"unsigned integer annotation range");return std::to_string(v);}
        if(column.integer_bits<32){int64_t bound=int64_t(1)<<(column.integer_bits-1);check(n>=-bound&&n<bound,"signed integer annotation range");}
      }
      return column.sql.type==FileSqlType::DATE?date_text(n):std::to_string(n);}
    case 2:{uint64_t v=b.le(8);int64_t n;std::memcpy(&n,&v,8);
      supported(column.integer_bits==0||column.integer_signed||v<=static_cast<uint64_t>(INT64_MAX),"unsigned integer exceeds SQL BIGINT");
      return column.sql.type==FileSqlType::DATETIME?timestamp_text(n,column.unit):std::to_string(n);}
    case 4:case 5:{double v;if(column.physical==4){uint32_t bits=static_cast<uint32_t>(b.le(4));float f;std::memcpy(&f,&bits,4);v=f;}else{uint64_t bits=b.le(8);std::memcpy(&v,&bits,8);}supported(std::isfinite(v),"non-finite Parquet floating value unsupported");std::ostringstream s;s.imbue(std::locale::classic());s<<std::setprecision(17)<<v;return s.str();}
    case 6:{uint64_t n=b.le(4);check(n<=MAX_CELL&&n<=b.left(),"Parquet string length limit");std::string s(reinterpret_cast<const char*>(b.p+b.pos),static_cast<size_t>(n));b.skip(static_cast<size_t>(n));supported(utf8(s),"binary/non-UTF8 Parquet value unsupported");return s;}
    default:throw Failure{OB_NOT_SUPPORTED,"unsupported Parquet physical type"};
  }
}
void load_page(int fd,Cursor &c,const Column &column){
  while(c.page_row==c.page_rows){check(c.offset<c.chunk.end,"column ended before row count");size_t head_len=static_cast<size_t>(std::min<uint64_t>(MAX_HEADER,c.chunk.end-c.offset));auto header_bytes=read_at(fd,c.offset,head_len);Bytes hb(header_bytes);Compact compact(hb);Node h=compact.value(12);int64_t type=number(h,1),uncompressed=number(h,2),compressed=number(h,3);check(uncompressed>=0&&compressed>=0&&uncompressed<=static_cast<int64_t>(MAX_PAGE)&&compressed<=static_cast<int64_t>(MAX_PAGE),"page size limit");c.offset+=hb.pos;check(static_cast<uint64_t>(compressed)<=c.chunk.end-c.offset,"page exceeds column chunk");auto raw=read_at(fd,c.offset,static_cast<size_t>(compressed));c.offset+=static_cast<uint64_t>(compressed);if(h.find(4))check(crc32(raw)==static_cast<uint32_t>(number(h,4)),"page CRC mismatch");if(type==1)continue;
    if(type==2){check(!c.dictionary_seen&&c.loaded==0,"duplicate/late dictionary");const Node &dh=field(h,7,12);int64_t count=number(dh,1);check(count>=0&&count<=static_cast<int64_t>(MAX_VALUES),"dictionary value limit");supported(number(dh,2)==0||number(dh,2)==2,"dictionary encoding unsupported");auto decoded=decompress(raw,static_cast<size_t>(uncompressed),c.chunk.codec);Bytes db(decoded);size_t bit=0,dict_bytes=0;for(int64_t i=0;i<count;++i){std::string v=plain(db,column,bit);dict_bytes+=v.size()+sizeof(std::string);check(dict_bytes<=MAX_PAGE,"decoded dictionary memory limit");c.dictionary.push_back(std::move(v));}check(db.left()==0,"trailing dictionary bytes");c.dictionary_seen=true;continue;}
    supported(type==0||type==3,"unsupported page type");const Node &dh=field(h,type==0?5:8,12);int64_t rows=number(dh,1);check(rows>0&&rows<=static_cast<int64_t>(MAX_VALUES)&&rows<=c.chunk.count-c.loaded,"data page value count");c.encoding=static_cast<int>(number(dh,type==0?2:4));supported(c.encoding==0||c.encoding==2||c.encoding==8||(c.encoding==3&&column.physical==0),"data value encoding unsupported");c.definitions.clear();c.indices.clear();c.page_rows=static_cast<size_t>(rows);c.page_row=0;c.values_pos=0;c.bool_bit=0;
    if(type==0){supported(number(dh,3)==3&&number(dh,4)==3,"legacy level encoding unsupported");auto decoded=decompress(raw,static_cast<size_t>(uncompressed),c.chunk.codec);Bytes levels(decoded);if(column.sql.nullable){size_t bytes=static_cast<size_t>(levels.le(4));check(bytes<=levels.left(),"definition level length");Bytes db(levels.p+levels.pos,bytes);c.definitions=hybrid(db,1,c.page_rows);check(db.left()==0,"trailing definition bytes");levels.skip(bytes);}c.values.assign(decoded.begin()+static_cast<ptrdiff_t>(levels.pos),decoded.end());}
    else{int64_t nulls=number(dh,2),num_rows=number(dh,3),def=number(dh,5),rep=number(dh,6);check(num_rows==rows&&nulls>=0&&nulls<=rows&&def>=0&&rep==0,"invalid flat V2 levels/counts");check(static_cast<uint64_t>(def)<=raw.size()&&def<=uncompressed,"V2 definition bounds");if(column.sql.nullable){Bytes db(raw.data(),static_cast<size_t>(def));c.definitions=hybrid(db,1,c.page_rows);check(db.left()==0,"trailing V2 definition bytes");}else check(def==0&&nulls==0,"required column has nulls");size_t actual=0;for(uint32_t d:c.definitions)if(!d)++actual;check(actual==static_cast<size_t>(nulls),"V2 null count mismatch");bool is_compressed=true;if(const Node *flag=dh.find(7)){check(flag->kind==1||flag->kind==2,"V2 compression boolean type");is_compressed=flag->value!=0;}std::vector<uint8_t> body(raw.begin()+static_cast<ptrdiff_t>(def),raw.end());c.values=decompress(body,static_cast<size_t>(uncompressed-def),is_compressed?c.chunk.codec:0);}
    size_t nonnull=c.page_rows;for(uint32_t d:c.definitions){check(d<=1,"invalid definition level");if(!d)--nonnull;}if(c.encoding==2||c.encoding==8){check(c.dictionary_seen,"missing dictionary");Bytes ids(c.values);if(ids.left()){unsigned width=ids.byte();check(width<=32,"dictionary index bit width");c.indices=hybrid(ids,width,nonnull);for(uint32_t id:c.indices)check(id<c.dictionary.size(),"dictionary index bounds");}else check(nonnull==0,"missing dictionary indices");check(ids.left()==0,"trailing dictionary index bytes");c.values.clear();}if(c.encoding==3){Bytes rb(c.values);size_t n=static_cast<size_t>(rb.le(4));check(n==rb.left(),"boolean RLE byte length");Bytes values(rb.p+rb.pos,n);c.indices=hybrid(values,1,nonnull);check(values.left()==0,"trailing boolean RLE bytes");c.values.clear();}c.loaded+=rows;return;
  }
}
FileSqlCell cell(int fd,Cursor &c,const Column &column){load_page(fd,c,column);FileSqlCell result;bool present=c.definitions.empty()||c.definitions[c.page_row]!=0;if(present){result.is_null=false;if(c.encoding==0){Bytes b(c.values);b.pos=c.values_pos;result.text=plain(b,column,c.bool_bit);c.values_pos=b.pos;}else{check(c.values_pos<c.indices.size(),"missing dictionary index");uint32_t index=c.indices[c.values_pos++];result.text=c.encoding==3?(index?"true":"false"):c.dictionary[index];}}++c.page_row;if(c.page_row==c.page_rows){if(c.encoding==0)check(c.values_pos+(c.bool_bit?1:0)==c.values.size(),"trailing plain page bytes");else check(c.values_pos==c.indices.size(),"trailing dictionary values");}return result;}
} // anonymous namespace
struct FileSqlParquetReader::Impl {
  int fd=-1,failure=0;std::string error;Metadata meta;std::vector<size_t> selected;std::vector<Cursor> cursors;size_t group=0;int64_t row=0,total=0;
  void reset(){cursors.clear();group=0;row=0;total=0;failure=0;}
  int fail(const Failure &f){error=f.text;failure=f.code;return f.code;}
  template<class F>int guard(F f){try{return f();}catch(const Failure&e){return fail(e);}catch(const std::bad_alloc&){return fail({OB_ALLOCATE_MEMORY_FAILED,"Parquet allocation failed"});}catch(const std::exception&){return fail({OB_INVALID_ARGUMENT,"Parquet decoder exception"});}}
};
FileSqlParquetReader::FileSqlParquetReader():impl_(new Impl){}
FileSqlParquetReader::~FileSqlParquetReader(){delete impl_;}
int FileSqlParquetReader::infer(int fd,FileSqlSchema &out){close();return impl_->guard([&](){Metadata m=metadata(fd);out=m.schema;impl_->error.clear();return OB_SUCCESS;});}
int FileSqlParquetReader::open(int fd,const FileSqlSchema &schema,const std::vector<int64_t>&projection){close();return impl_->guard([&](){Metadata m=metadata(fd);check(m.schema.row_count==schema.row_count&&m.schema.columns.size()==schema.columns.size(),"Parquet schema changed");for(size_t i=0;i<schema.columns.size();++i){const auto&a=schema.columns[i];const auto&b=m.schema.columns[i];check(a.name==b.name&&a.type==b.type&&a.nullable==b.nullable,"Parquet schema changed");}std::vector<size_t> selected;if(projection.empty()){for(size_t i=0;i<schema.columns.size();++i)selected.push_back(i);}else for(int64_t i:projection){check(i>=0&&static_cast<size_t>(i)<schema.columns.size(),"Parquet projection index");check(std::find(selected.begin(),selected.end(),static_cast<size_t>(i))==selected.end(),"duplicate projection");selected.push_back(static_cast<size_t>(i));}impl_->meta=std::move(m);impl_->selected=std::move(selected);impl_->fd=fd;impl_->error.clear();return OB_SUCCESS;});}
int FileSqlParquetReader::next(FileSqlRow &out){return impl_->guard([&](){check(impl_->fd>=0,"Parquet reader not open");if(impl_->failure)return impl_->failure;if(impl_->total==impl_->meta.schema.row_count)return OB_ITER_END;while(impl_->group<impl_->meta.groups.size()&&impl_->row==impl_->meta.groups[impl_->group].rows){++impl_->group;impl_->row=0;impl_->cursors.clear();}check(impl_->group<impl_->meta.groups.size(),"missing rowgroup");const Group &g=impl_->meta.groups[impl_->group];if(impl_->cursors.empty())for(size_t i:impl_->selected){Cursor c;c.chunk=g.chunks[i];c.offset=c.chunk.begin;impl_->cursors.push_back(std::move(c));}FileSqlRow result;result.cells.resize(impl_->meta.columns.size());size_t memory=0;for(size_t i=0;i<impl_->selected.size();++i){size_t ordinal=impl_->selected[i];result.cells[ordinal]=cell(impl_->fd,impl_->cursors[i],impl_->meta.columns[ordinal]);memory+=impl_->cursors[i].memory();check(memory<=MAX_MEMORY,"live page/dictionary memory limit");}++impl_->row;++impl_->total;out=std::move(result);return OB_SUCCESS;});}
int FileSqlParquetReader::rescan(){return impl_->guard([&](){check(impl_->fd>=0,"Parquet reader not open");impl_->reset();impl_->error.clear();return OB_SUCCESS;});}
void FileSqlParquetReader::close(){impl_->fd=-1;impl_->reset();impl_->selected.clear();impl_->meta=Metadata();}
const std::string &FileSqlParquetReader::error()const{return impl_->error;}
} }
