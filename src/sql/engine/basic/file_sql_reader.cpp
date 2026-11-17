#include <rapidjson/reader.h>
#include "sql/engine/basic/file_sql_parquet.h"
#include <map>
#include "sql/engine/basic/file_sql_reader_impl.h"
namespace oceanbase { namespace sql {
using namespace common;
using namespace file_sql_detail;
struct FileSqlReader::Impl {
  std::string error, path, root;
  FileSqlOptions options;
  FileSqlSchema schema;
  std::vector<bool> projected;
  FILE *file=nullptr;
  int64_t record=0, line=1, scanned=0;
  size_t bytes=0;
  bool opened=false, reading_header=false;
  int64_t diagnostic_record=1, diagnostic_line=1;
  size_t metadata_bytes=0;
  std::map<std::string,size_t> json_columns;
  std::vector<int> json_kinds;
  GlobState glob;
  FileSqlParquetReader parquet;
  struct JsonRecord : rapidjson::BaseReaderHandler<rapidjson::UTF8<>,JsonRecord> {
    struct Item { std::string key; FileSqlCell cell; FileSqlType type=FileSqlType::NULL_VALUE; int kind=0; };
    std::vector<Item> items;
    std::set<std::string> keys;
    std::string key, why;
    bool started=false, ended=false, pending=false;
    bool reject(const char *message) { why=message; return false; }
    bool StartObject() {
      if(started) return reject("nested JSON objects are unsupported");
      started=true; return true;
    }
    bool EndObject(rapidjson::SizeType) { ended=true; return !pending; }
    bool StartArray() { return reject("nested JSON arrays are unsupported"); }
    bool Key(const char *s,rapidjson::SizeType n,bool) {
      if(!n || n>MAX_METADATA || items.size()>=MAX_COLUMNS) return reject("empty JSON key or column limit exceeded");
      key.assign(s,n);
      if(key.find('\0')!=std::string::npos || !keys.insert(lower(key)).second) return reject("duplicate or invalid JSON key");
      pending=true; return true;
    }
    bool value(std::string text,FileSqlType type,int kind,bool null=false) {
      if(!started || ended || !pending) return reject("JSONL record must be an object");
      Item item; item.key=key; item.cell.is_null=null; item.cell.text=std::move(text); item.type=type; item.kind=kind;
      items.push_back(std::move(item)); pending=false; return true;
    }
    bool Null() { return value("",FileSqlType::NULL_VALUE,0,true); }
    bool Bool(bool b) { return value(b?"true":"false",FileSqlType::BOOLEAN,2); }
    bool String(const char *s,rapidjson::SizeType n,bool) {
      std::string text(s,n); FileSqlType type=FileSqlType::VARCHAR;
      if(text.size()>=10 && digits(text,0,4) && text[4]=='-' && text[7]=='-') {
        if(classify(text,type)!=OB_SUCCESS) return reject("invalid JSON date/datetime");
      }
      return value(std::move(text),type,3);
    }
    bool RawNumber(const char *s,rapidjson::SizeType n,bool) {
      std::string text(s,n); FileSqlType type;
      if(classify(text,type)!=OB_SUCCESS || (type!=FileSqlType::BIGINT && type!=FileSqlType::DOUBLE))
        return reject("JSON number is outside BIGINT or finite DOUBLE range");
      return value(std::move(text),type,1);
    }
  };
  ~Impl() { reset(); }
  void reset() { parquet.close(); if(file) std::fclose(file); file=nullptr; opened=false; record=0; line=1; scanned=0; }
  int fail(int code,const std::string &what,size_t col=0) {
    error=path+": record "+std::to_string(diagnostic_record)+", line "+std::to_string(diagnostic_line)+", column "+std::to_string(col+1)+": "+what;
    return code;
  }
  int rewind_file() {
    if(std::fseek(file,0,SEEK_SET)!=0) return fail(OB_IO_ERROR,"cannot rewind file");
    unsigned char bom[3]; size_t n=std::fread(bom,1,3,file);
    if(std::ferror(file)) return fail(OB_IO_ERROR,"cannot read file prefix");
    if(n!=3 || bom[0]!=0xef || bom[1]!=0xbb || bom[2]!=0xbf)
      if(std::fseek(file,0,SEEK_SET)!=0) return fail(OB_IO_ERROR,"cannot rewind file");
    std::clearerr(file); record=0; line=1; scanned=0; return OB_SUCCESS;
  }
  int configure(const std::string &p,const FileSqlOptions &o,const std::string &r) {
    reset(); error.clear(); path=p; root=r; options=o; options.format=lower(o.format);
    if(options.format=="ndjson" || options.format=="jsonl") options.format="json";
    if(options.format!="csv" && options.format!="json" && options.format!="glob" && options.format!="parquet") return fail(OB_NOT_SUPPORTED,"unsupported file format");
    metadata_bytes=0; json_columns.clear(); json_kinds.clear();
    if(o.sample_size!=-1) return fail(OB_NOT_SUPPORTED,"sample_size must be -1: full schema inference is required");
    schema=FileSqlSchema();
    if(options.format=="glob") {
      std::string why; int ret=glob.discover(path,root,schema.fingerprint,why);
      if(ret) return fail(ret,why);
      schema.columns.push_back({"file",FileSqlType::VARCHAR,false}); schema.row_count=glob.files.size();
      return OB_SUCCESS;
    }
    if(o.delimiter==0 || o.quote==0 || o.escape==0 || o.delimiter==o.quote ||
       o.delimiter=='\r' || o.delimiter=='\n' || o.quote=='\r' || o.quote=='\n' ||
       o.escape=='\r' || o.escape=='\n' || o.nullstr.size()>MAX_RECORD || !utf8(o.nullstr))
      return fail(OB_INVALID_ARGUMENT,"invalid CSV options");
    std::string canonical,why;
    int fd=secure_open(path,root,false,canonical,why);
    if(fd<0) return fail(OB_INVALID_ARGUMENT,why);
    file=::fdopen(fd,"rb"); if(!file) { ::close(fd); return fail(OB_IO_ERROR,"fdopen failed"); }
    struct stat st; if(::fstat(fd,&st)!=0) return fail(OB_IO_ERROR,"fstat failed");
    schema=FileSqlSchema(); schema.fingerprint=fingerprint(st); return options.format=="parquet" ? OB_SUCCESS : rewind_file();
  }
  int unchanged() {
    if(options.format=="glob") {
      std::string why; int ret=glob.validate(root,why,glob.position==glob.files.size());
      return ret?fail(ret,why):OB_SUCCESS;
    }
    struct stat st;
    if(!file || ::fstat(::fileno(file),&st)!=0) return fail(OB_IO_ERROR,"cannot validate opened file");
    if(!equal(fingerprint(st),schema.fingerprint)) return fail(OB_INVALID_DATA,"file fingerprint changed");
    std::string canonical,why;
    int fd=secure_open(path,root,false,canonical,why);
    if(fd<0) return fail(OB_INVALID_DATA,"file path changed: "+why);
    bool ok=::fstat(fd,&st)==0 && equal(fingerprint(st),schema.fingerprint); ::close(fd);
    return ok?OB_SUCCESS:fail(OB_INVALID_DATA,"file path fingerprint changed");
  }
  int get() {
    int c=std::fgetc(file); if(c!=EOF && ++bytes>MAX_RECORD) return -2; return c;
  }
  int csv(FileSqlRow &row) {
    diagnostic_record=record+1; diagnostic_line=line;
    row.cells.clear(); bytes=0; std::string text; bool quoted=false,after=false,start=true,any=false,field_quoted=false;
    auto field=[&]() -> int {
      if(row.cells.size()>=MAX_COLUMNS) return fail(OB_SIZE_OVERFLOW,"column limit exceeded",row.cells.size());
      if(!utf8(text)) return fail(OB_INVALID_DATA,"invalid UTF-8",row.cells.size());
      FileSqlCell c; c.is_null=!reading_header && !field_quoted && (text.empty() || text==options.nullstr);
      if(!c.is_null) c.text.swap(text); else text.clear();
      row.cells.push_back(std::move(c)); start=true; after=false; field_quoted=false; return OB_SUCCESS;
    };
    while(true) {
      int c=get();
      if(c==-2) return fail(OB_SIZE_OVERFLOW,"record exceeds 8 MiB limit",row.cells.size());
      if(c==EOF) {
        if(std::ferror(file)) return fail(OB_IO_ERROR,"read failed");
        if(quoted) return fail(OB_INVALID_DATA,"unterminated quoted CSV field",row.cells.size());
        if(!any) return OB_ITER_END;
        int ret=field(); ++record; return ret;
      }
      any=true;
      if(c==0) return fail(OB_INVALID_DATA,"NUL byte in CSV",row.cells.size());
      if(quoted) {
        if(c==options.quote) { quoted=false; after=true; }
        else if(options.escape!=options.quote && c==options.escape) {
          c=get(); if(c!=options.quote && c!=options.escape) return fail(OB_INVALID_DATA,"invalid CSV escape",row.cells.size());
          text+=static_cast<char>(c);
        } else if(c=='\r') {
          c=get(); if(c!='\n') return fail(OB_INVALID_DATA,"bare CR in CSV",row.cells.size());
          ++line; text+='\n';
        } else { if(c=='\n') ++line; text+=static_cast<char>(c); }
      } else if(after && c==options.quote) { text+=options.quote; quoted=true; after=false; }
      else if(c==options.delimiter) { int ret=field(); if(ret) return ret; }
      else if(c=='\n' || c=='\r') {
        if(c=='\r' && get()!='\n') return fail(OB_INVALID_DATA,"bare CR in CSV",row.cells.size());
        ++line; int ret=field(); ++record; return ret;
      } else if(after) return fail(OB_INVALID_DATA,"unexpected character after closing quote",row.cells.size());
      else if(c==options.quote) {
        if(!start) return fail(OB_INVALID_DATA,"quote in unquoted CSV field",row.cells.size());
        quoted=true; field_quoted=true; start=false;
      } else { text+=static_cast<char>(c); start=false; }
    }
  }
  int header() {
    FileSqlRow row; reading_header=options.header; int ret=csv(row); reading_header=false;
    if(ret==OB_ITER_END) return fail(OB_INVALID_DATA,"CSV has no header or data");
    if(ret) return ret;
    std::set<std::string> used; size_t names=0;
    for(size_t i=0;i<row.cells.size();++i) {
      std::string base=options.header && !row.cells[i].text.empty()?row.cells[i].text:"column"+std::to_string(i);
      std::string name=base; int suffix=2;
      while(used.count(lower(name))) name=base+"__"+std::to_string(suffix++);
      names+=name.size(); if(names>MAX_METADATA) return fail(OB_SIZE_OVERFLOW,"schema metadata limit exceeded");
      used.insert(lower(name)); schema.columns.push_back({name,FileSqlType::NULL_VALUE,false});
    }
    if(!options.header) return rewind_file();
    return OB_SUCCESS;
  }
  int json(FileSqlRow &row,std::vector<FileSqlType> &types) {
    std::string text;
    while(true) {
      text.clear(); bytes=0;
      while(true) {
        int c=get();
        if(c==-2) return fail(OB_SIZE_OVERFLOW,"JSONL record exceeds 8 MiB limit");
        if(c==EOF) {
          if(std::ferror(file)) return fail(OB_IO_ERROR,"read failed");
          if(text.empty()) return OB_ITER_END;
          break;
        }
        if(c=='\n') { ++line; break; }
        if(c==0) return fail(OB_INVALID_DATA,"NUL byte in JSONL");
        text+=static_cast<char>(c);
      }
      if(!text.empty() && text.back()=='\r') text.pop_back();
      if(text.find('\r')!=std::string::npos) return fail(OB_INVALID_DATA,"bare CR in JSONL");
      if(text.find_first_not_of(" \t")==std::string::npos) continue;
      break;
    }
    JsonRecord handler; rapidjson::Reader parser; rapidjson::StringStream input(text.c_str());
    if(!parser.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseNumbersAsStringsFlag | rapidjson::kParseIterativeFlag>(input,handler) || !handler.started || !handler.ended)
      return fail(OB_INVALID_DATA,handler.why.empty()?(parser.GetParseErrorCode()==rapidjson::kParseErrorNumberTooBig?"JSON number is outside finite DOUBLE range":"invalid JSONL syntax at byte "+std::to_string(parser.GetErrorOffset())):handler.why);
    row.cells.assign(schema.columns.size(),FileSqlCell()); types.assign(schema.columns.size(),FileSqlType::NULL_VALUE);
    for(auto &item:handler.items) {
      auto it=json_columns.find(item.key); size_t ordinal=0;
      if(it==json_columns.end()) {
        if(opened) return fail(OB_INVALID_DATA,"JSONL key not found in inferred schema");
        if(schema.columns.size()>=MAX_COLUMNS || metadata_bytes+item.key.size()>MAX_METADATA)
          return fail(OB_SIZE_OVERFLOW,"JSONL schema metadata limit exceeded");
        for(const auto &column:schema.columns)
          if(lower(column.name)==lower(item.key)) return fail(OB_INVALID_DATA,"case-conflicting JSONL keys");
        metadata_bytes+=item.key.size(); ordinal=schema.columns.size(); json_columns[item.key]=ordinal;
        schema.columns.push_back({item.key,FileSqlType::NULL_VALUE,schema.row_count>0}); json_kinds.push_back(0);
        row.cells.emplace_back(); types.push_back(FileSqlType::NULL_VALUE);
      } else ordinal=it->second;
      if(item.kind!=0) {
        if(json_kinds[ordinal]!=0 && json_kinds[ordinal]!=item.kind) return fail(OB_INVALID_DATA,"conflicting JSON scalar types",ordinal);
        json_kinds[ordinal]=item.kind;
      }
      row.cells[ordinal]=std::move(item.cell); types[ordinal]=item.type;
    }
    ++record; return OB_SUCCESS;
  }
  int parse(FileSqlRow &row,std::vector<FileSqlType> &types) {
    if(options.format=="glob") {
      if(glob.position==glob.files.size()) return OB_ITER_END;
      row.cells.assign(1,FileSqlCell()); row.cells[0].is_null=false; row.cells[0].text=glob.files[glob.position++].path;
      types.assign(1,FileSqlType::VARCHAR); ++record; return OB_SUCCESS;
    }
    if(options.format=="json") return json(row,types);
    int ret=csv(row); if(ret) return ret;
    if(row.cells.size()!=schema.columns.size()) return fail(OB_INVALID_DATA,"CSV row width differs from schema",std::min(row.cells.size(),schema.columns.size()));
    types.assign(row.cells.size(),FileSqlType::NULL_VALUE);
    for(size_t i=0;i<row.cells.size();++i) if(!row.cells[i].is_null) {
      ret=classify(row.cells[i].text,types[i]); if(ret) return fail(ret,"invalid scalar or value out of range",i);
    }
    return OB_SUCCESS;
  }
};
FileSqlReader::FileSqlReader():impl_(new Impl) {}
FileSqlReader::~FileSqlReader() { delete impl_; }
int FileSqlReader::infer(const std::string &path,const FileSqlOptions &o,const std::string &root,FileSqlSchema &out) {
  try {
    int ret=impl_->configure(path,o,root); if(ret) return ret;
    if(impl_->options.format=="glob") { out=impl_->schema; impl_->reset(); return OB_SUCCESS; }
    if(impl_->options.format=="parquet") {
      FileSqlSchema metadata;
      ret=impl_->parquet.infer(::fileno(impl_->file),metadata);
      if(ret) return impl_->fail(ret,impl_->parquet.error());
      metadata.fingerprint=impl_->schema.fingerprint;
      if((ret=impl_->unchanged())!=OB_SUCCESS) return ret;
      out=metadata; impl_->reset(); return OB_SUCCESS;
    }
    if(impl_->options.format=="csv" && (ret=impl_->header())!=OB_SUCCESS) return ret;
    FileSqlRow row; std::vector<FileSqlType> types;
    while((ret=impl_->parse(row,types))==OB_SUCCESS) {
      if(impl_->schema.row_count==INT64_MAX) return impl_->fail(OB_SIZE_OVERFLOW,"row count overflow");
      ++impl_->schema.row_count;
      for(size_t i=0;i<types.size();++i) {
        auto &col=impl_->schema.columns[i]; col.nullable=col.nullable || row.cells[i].is_null;
        col.type=merge_type(col.type,types[i]);
        if(types[i]==FileSqlType::DATETIME){
          size_t dot=row.cells[i].text.find('.');
          if(dot!=std::string::npos)col.scale=std::max(col.scale,static_cast<int>(row.cells[i].text.size()-dot-1));
        }
      }
    }
    if(ret!=OB_ITER_END || (ret=impl_->unchanged())!=OB_SUCCESS) return ret;
    out=impl_->schema; impl_->reset(); return OB_SUCCESS;
  } catch(const std::bad_alloc&) { return impl_->fail(OB_ALLOCATE_MEMORY_FAILED,"allocation failed"); }
}
int FileSqlReader::open(const std::string &path,const FileSqlOptions &o,const std::string &root,
                        const FileSqlSchema &expected,const std::vector<int64_t> &projection) {
  try {
    int ret=impl_->configure(path,o,root); if(ret) return ret;
    if(!equal(impl_->schema.fingerprint,expected.fingerprint)) return impl_->fail(OB_INVALID_DATA,"file fingerprint changed since inference");
    if(impl_->options.format=="parquet") {
      ret=impl_->parquet.open(::fileno(impl_->file),expected,projection);
      if(ret) return impl_->fail(ret,impl_->parquet.error());
      impl_->schema=expected; impl_->opened=true; return impl_->unchanged();
    }
    if(impl_->options.format=="csv" && (ret=impl_->header())!=OB_SUCCESS) return ret;
    if(impl_->options.format=="csv") {
      if(impl_->schema.columns.size()!=expected.columns.size()) return impl_->fail(OB_INVALID_DATA,"schema width changed");
      for(size_t i=0;i<expected.columns.size();++i)
        if(impl_->schema.columns[i].name!=expected.columns[i].name) return impl_->fail(OB_INVALID_DATA,"schema header changed",i);
    } else {
      impl_->json_kinds.assign(expected.columns.size(),0);
      for(size_t i=0;i<expected.columns.size();++i) impl_->json_columns[expected.columns[i].name]=i;
    }
    impl_->schema=expected; impl_->projected.assign(expected.columns.size(),false);
    for(int64_t i:projection) {
      if(i<0 || static_cast<size_t>(i)>=expected.columns.size()) return impl_->fail(OB_INVALID_ARGUMENT,"projection ordinal out of range");
      impl_->projected[i]=true;
    }
    impl_->opened=true; return impl_->unchanged();
  } catch(const std::bad_alloc&) { return impl_->fail(OB_ALLOCATE_MEMORY_FAILED,"allocation failed"); }
}
int FileSqlReader::next(FileSqlRow &row) {
  row.cells.clear();
  try {
    if(!impl_->opened) return impl_->fail(OB_NOT_INIT,"reader is not open");
    int ret=impl_->unchanged(); if(ret) return ret;
    if(impl_->options.format=="parquet") {
      ret=impl_->parquet.next(row);
      if(ret!=OB_SUCCESS && ret!=OB_ITER_END) return impl_->fail(ret,impl_->parquet.error());
      int check=impl_->unchanged(); return check==OB_SUCCESS?ret:check;
    }
    std::vector<FileSqlType> types;
    ret=impl_->parse(row,types);
    if(ret==OB_ITER_END) {
      if(impl_->scanned!=impl_->schema.row_count) return impl_->fail(OB_INVALID_DATA,"row count changed");
      return (ret=impl_->unchanged())==OB_SUCCESS?OB_ITER_END:ret;
    }
    if(ret) { row.cells.clear(); return ret; }
    ++impl_->scanned;
    if(impl_->scanned>impl_->schema.row_count) return impl_->fail(OB_INVALID_DATA,"row count changed");
    for(size_t i=0;i<row.cells.size();++i) {
      if(!row.cells[i].is_null && merge_type(impl_->schema.columns[i].type,types[i])!=impl_->schema.columns[i].type)
        return impl_->fail(OB_INVALID_DATA,"scalar conflicts with inferred schema",i);
      if(!impl_->projected[i]) row.cells[i]=FileSqlCell();
    }
    return impl_->unchanged();
  } catch(const std::bad_alloc&) { return impl_->fail(OB_ALLOCATE_MEMORY_FAILED,"allocation failed"); }
}
int FileSqlReader::rescan() {
  if(!impl_->opened) return impl_->fail(OB_NOT_INIT,"reader is not open");
  int ret=impl_->unchanged(); if(ret) return ret;
  if(impl_->options.format=="glob") {
    std::string why; ret=impl_->glob.validate(impl_->root,why,true); if(ret) return impl_->fail(ret,why);
    impl_->glob.position=0; impl_->scanned=0; impl_->record=0; return impl_->unchanged();
  }
  if(impl_->options.format=="parquet") {
    ret=impl_->parquet.rescan();
    return ret==OB_SUCCESS ? impl_->unchanged() : impl_->fail(ret,impl_->parquet.error());
  }
  ret=impl_->rewind_file(); if(ret) return ret;
  if(impl_->options.format=="csv" && impl_->options.header) { FileSqlRow header; ret=impl_->csv(header); if(ret) return ret; }
  return impl_->unchanged();
}
void FileSqlReader::close() { impl_->reset(); }
const std::string &FileSqlReader::error() const { return impl_->error; }
} }
