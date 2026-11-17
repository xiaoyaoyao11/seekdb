// MiniVectorDB - a self-contained lightweight vector database.
//
// Protocol: JSON Lines over stdin/stdout (see STDIO_PROTOCOL.md).
//   startup -> {"type":"ready"}
//   request -> {"id":...,"op":"execute","sql":"...","search_mode":"exact|ann"}
//   reply   -> {"id":...,"ok":true,"columns":[...],"rows":[...]}
//              {"id":...,"ok":true,"affected_rows":N}
//              {"id":...,"ok":false,"error":{"code":"...","message":"..."}}
//   {"id":...,"op":"shutdown"} -> {"id":...,"ok":true}, then exit 0.
//
// All persistent state lives in /data (or --data-dir).  A single append-only
// WAL holds committed transactions; every commit is flushed and fsynced before
// its success response is emitted.  Recovery replays complete, CRC-verified
// records and truncates any torn tail, which gives all-or-nothing commits even
// when the process is killed mid-recovery.
//
// No third-party storage engine, SQL engine or ANN library is used.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
static void platform_fsync(FILE *f) {
  std::fflush(f);
  _commit(_fileno(f));
}
static void platform_binary_stdout() { _setmode(_fileno(stdout), _O_BINARY); }
static void platform_truncate(const std::string &path, std::uint64_t len) {
  int fd = _open(path.c_str(), _O_RDWR | _O_BINARY);
  if (fd >= 0) { _chsize_s(fd, static_cast<__int64>(len)); _close(fd); }
}
#else
#include <unistd.h>
static void platform_fsync(FILE *f) {
  std::fflush(f);
  fsync(fileno(f));
}
static void platform_binary_stdout() {}
static void platform_truncate(const std::string &path, std::uint64_t len) {
  // truncate() reports failure through its return value; the caller has
  // already checked the file state, so the result is only worth logging.
  if (truncate(path.c_str(), static_cast<off_t>(len)) != 0) {
    std::fprintf(stderr, "minivectordb: cannot truncate %s\n", path.c_str());
  }
}
#endif

// ---------------------------------------------------------------- JSON -----

struct JV {
  enum Type { NUL, BOOL, INT, DBL, STR, ARR, OBJ } type = NUL;
  bool b = false;
  long long i = 0;
  double d = 0.0;
  std::string s;
  std::vector<JV> arr;
  std::vector<std::pair<std::string, JV>> obj;

  static JV nullv() { return JV(); }
  static JV boolv(bool v) { JV x; x.type = BOOL; x.b = v; return x; }
  static JV intv(long long v) { JV x; x.type = INT; x.i = v; return x; }
  static JV dblv(double v) { JV x; x.type = DBL; x.d = v; return x; }
  static JV strv(const std::string &v) { JV x; x.type = STR; x.s = v; return x; }
  static JV arrv() { JV x; x.type = ARR; return x; }
  static JV objv() { JV x; x.type = OBJ; return x; }
  const JV *find(const std::string &k) const {
    for (const auto &kv : obj) if (kv.first == k) return &kv.second;
    return nullptr;
  }
};

static void append_utf8(std::string &out, std::uint32_t cp) {
  if (cp < 0x80) out += static_cast<char>(cp);
  else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

static void append_json_string(std::string &out, const std::string &s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", c); out += buf; }
        else out += static_cast<char>(c);
    }
  }
  out += '"';
}

static std::string dump_double(double d) {
  if (!std::isfinite(d)) return "0.0";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", d);
  std::string s(buf);
  if (s.find_first_of(".eE") == std::string::npos) s += ".0";
  return s;
}

static std::string jdump(const JV &v) {
  std::string out;
  switch (v.type) {
    case JV::NUL: out += "null"; break;
    case JV::BOOL: out += v.b ? "true" : "false"; break;
    case JV::INT: out += std::to_string(v.i); break;
    case JV::DBL: out += dump_double(v.d); break;
    case JV::STR: append_json_string(out, v.s); break;
    case JV::ARR: {
      out += '[';
      for (size_t i = 0; i < v.arr.size(); ++i) {
        if (i) out += ',';
        out += jdump(v.arr[i]);
      }
      out += ']';
      break;
    }
    case JV::OBJ: {
      out += '{';
      for (size_t i = 0; i < v.obj.size(); ++i) {
        if (i) out += ',';
        append_json_string(out, v.obj[i].first);
        out += ':';
        out += jdump(v.obj[i].second);
      }
      out += '}';
      break;
    }
  }
  return out;
}

struct JParser {
  const std::string &s;
  size_t p = 0;
  bool ok = true;
  explicit JParser(const std::string &text) : s(text) {}

  void ws() {
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p;
  }
  bool lit(const char *word) {
    size_t n = std::strlen(word);
    if (s.compare(p, n, word) == 0) { p += n; return true; }
    return false;
  }
  bool hex4(std::uint32_t &cp) {
    if (p + 4 > s.size()) return false;
    cp = 0;
    for (int k = 0; k < 4; ++k) {
      char c = s[p++];
      cp <<= 4;
      if (c >= '0' && c <= '9') cp |= (c - '0');
      else if (c >= 'a' && c <= 'f') cp |= (c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') cp |= (c - 'A' + 10);
      else return false;
    }
    return true;
  }
  bool parse_string(std::string &out) {
    if (p >= s.size() || s[p] != '"') return false;
    ++p;
    while (p < s.size()) {
      char c = s[p++];
      if (c == '"') return true;
      if (c == '\\') {
        if (p >= s.size()) return false;
        char e = s[p++];
        switch (e) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u': {
            std::uint32_t cp = 0;
            if (!hex4(cp)) return false;
            if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 <= s.size() && s[p] == '\\' && s[p + 1] == 'u') {
              p += 2;
              std::uint32_t lo = 0;
              if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            append_utf8(out, cp);
            break;
          }
          default: return false;
        }
      } else {
        out += c;
      }
    }
    return false;
  }
  bool number(JV &out) {
    size_t start = p;
    if (p < s.size() && s[p] == '-') ++p;
    if (p >= s.size()) return false;
    if (s[p] == '0') ++p;
    else if (s[p] >= '1' && s[p] <= '9') { while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) ++p; }
    else return false;
    bool isdbl = false;
    if (p < s.size() && s[p] == '.') {
      isdbl = true; ++p;
      while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) ++p;
    }
    if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
      isdbl = true; ++p;
      if (p < s.size() && (s[p] == '+' || s[p] == '-')) ++p;
      while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) ++p;
    }
    std::string tok = s.substr(start, p - start);
    if (isdbl) {
      try { out = JV::dblv(std::stod(tok)); }
      catch (...) { return false; }
    } else {
      try { out = JV::intv(std::stoll(tok)); }
      catch (...) {
        try { out = JV::dblv(std::stod(tok)); } catch (...) { return false; }
      }
    }
    return true;
  }
  bool value(JV &out) {
    ws();
    if (p >= s.size()) return false;
    char c = s[p];
    if (c == '"') { std::string t; if (!parse_string(t)) return false; out = JV::strv(t); return true; }
    if (c == '{') {
      ++p; out = JV::objv(); ws();
      if (p < s.size() && s[p] == '}') { ++p; return true; }
      for (;;) {
        ws();
        std::string k;
        if (!parse_string(k)) return false;
        ws();
        if (p >= s.size() || s[p] != ':') return false;
        ++p;
        JV v;
        if (!value(v)) return false;
        out.obj.emplace_back(std::move(k), std::move(v));
        ws();
        if (p < s.size() && s[p] == ',') { ++p; continue; }
        if (p < s.size() && s[p] == '}') { ++p; return true; }
        return false;
      }
    }
    if (c == '[') {
      ++p; out = JV::arrv(); ws();
      if (p < s.size() && s[p] == ']') { ++p; return true; }
      for (;;) {
        JV v;
        if (!value(v)) return false;
        out.arr.push_back(std::move(v));
        ws();
        if (p < s.size() && s[p] == ',') { ++p; continue; }
        if (p < s.size() && s[p] == ']') { ++p; return true; }
        return false;
      }
    }
    if (lit("true")) { out = JV::boolv(true); return true; }
    if (lit("false")) { out = JV::boolv(false); return true; }
    if (lit("null")) { out = JV::nullv(); return true; }
    if (c == '-' || (c >= '0' && c <= '9')) return number(out);
    return false;
  }
};

// ------------------------------------------------------------- utilities ----

static std::string upper(std::string s) {
  for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}
static bool ieq(const std::string &a, const char *b) {
  return upper(a) == upper(std::string(b));
}
static std::string trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}
static size_t utf8_chars(const std::string &s) {
  size_t n = 0;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t k = c < 0x80 ? 1 : (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4));
    i += k; ++n;
  }
  return n;
}
static std::uint64_t fnv1a(const std::string &s) {
  std::uint64_t h = 14695981039346656037ULL;
  for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
  return h;
}

struct DbError {
  std::string code;
  std::string message;
  DbError(std::string c, std::string m) : code(std::move(c)), message(std::move(m)) {}
};

// ------------------------------------------------------------- data model --

enum ColType { T_BIGINT = 0, T_VARCHAR = 1, T_VECTOR = 2 };

struct Column {
  std::string name;
  int type = T_BIGINT;
  int len = 0;
  bool pk = false;
};

struct Cell {
  long long i = 0;
  std::string s;
  std::vector<float> v;
};
using Row = std::vector<Cell>;

struct Table {
  std::string name;
  std::vector<Column> cols;
  int pk_idx = -1;
  int vec_idx = -1;
  bool has_index = false;
  std::map<long long, Row> rows;
  int col_index(const std::string &n) const {
    for (size_t i = 0; i < cols.size(); ++i) if (ieq(cols[i].name, n.c_str())) return static_cast<int>(i);
    return -1;
  }
};

// kind: 0 create_table, 1 put, 2 del, 3 create_index
struct Op {
  int kind = 0;
  std::string table;
  long long id = 0;
  Row row;
  std::vector<Column> cols;
};

struct Txn {
  bool active = false;
  std::vector<Op> ops;
  std::map<std::string, std::map<long long, std::optional<Row>>> changes;
  std::map<std::string, Table> new_tables;
};

struct ViewRow {
  long long id;
  const Row *row;
};

// WAL serialisation helpers -------------------------------------------------

static JV cell_to_json(const Cell &c, int type) {
  if (type == T_BIGINT) return JV::intv(c.i);
  if (type == T_VARCHAR) return JV::strv(c.s);
  JV a = JV::arrv();
  for (float f : c.v) a.arr.push_back(JV::dblv(static_cast<double>(f)));
  return a;
}

// WAL floats are float32 values; 9 significant digits round-trip exactly and
// keep the log (and therefore the measured disk I/O B) much smaller than %.17g.
static void append_cell_wal(std::string &out, const Cell &c, int type) {
  if (type == T_BIGINT) {
    out += std::to_string(c.i);
  } else if (type == T_VARCHAR) {
    append_json_string(out, c.s);
  } else {
    out += '[';
    char buf[32];
    for (size_t i = 0; i < c.v.size(); ++i) {
      if (i) out += ',';
      std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(c.v[i]));
      char *end = buf;
      bool has_mark = false;
      for (; *end; ++end) if (*end == '.' || *end == 'e' || *end == 'E') { has_mark = true; break; }
      if (!has_mark) { *end++ = '.'; *end++ = '0'; *end = '\0'; }
      out += buf;
    }
    out += ']';
  }
}

static std::string ops_to_json(const std::vector<Op> &ops,
                               const std::map<std::string, std::vector<Column>> &defs) {
  std::string out = "[";
  for (size_t oi = 0; oi < ops.size(); ++oi) {
    const Op &op = ops[oi];
    if (oi) out += ',';
    out += '[';
    out += std::to_string(op.kind);
    out += ',';
    append_json_string(out, op.table);
    if (op.kind == 0) {
      out += ",[";
      for (size_t i = 0; i < op.cols.size(); ++i) {
        if (i) out += ',';
        const Column &c = op.cols[i];
        out += '[';
        append_json_string(out, c.name);
        out += ',';
        out += std::to_string(c.type);
        out += ',';
        out += std::to_string(c.len);
        out += ',';
        out += c.pk ? "1" : "0";
        out += ']';
      }
      out += ']';
    } else if (op.kind == 1) {
      out += ',';
      out += std::to_string(op.id);
      out += ",[";
      auto it = defs.find(op.table);
      if (it != defs.end()) {
        for (size_t i = 0; i < it->second.size(); ++i) {
          if (i) out += ',';
          append_cell_wal(out, op.row[i], it->second[i].type);
        }
      }
      out += ']';
    } else if (op.kind == 2) {
      out += ',';
      out += std::to_string(op.id);
    }
    out += ']';
  }
  out += ']';
  return out;
}

// -------------------------------------------------------------- SQL lexer --

struct Tok {
  int t = 0;  // 0 ident, 1 number, 2 string, 3 punct, 4 end
  std::string v;
};

static std::vector<Tok> lex(const std::string &sql) {
  std::vector<Tok> out;
  size_t p = 0;
  while (p < sql.size()) {
    unsigned char c = static_cast<unsigned char>(sql[p]);
    if (std::isspace(c)) { ++p; continue; }
    if (std::isalpha(c) || c == '_') {
      size_t a = p++;
      while (p < sql.size()) {
        unsigned char d = static_cast<unsigned char>(sql[p]);
        if (std::isalnum(d) || d == '_' || d == '$') ++p; else break;
      }
      out.push_back({0, sql.substr(a, p - a)});
      continue;
    }
    if (std::isdigit(c)) {
      size_t a = p;
      while (p < sql.size() && std::isdigit(static_cast<unsigned char>(sql[p]))) ++p;
      if (p < sql.size() && sql[p] == '.') {
        ++p;
        while (p < sql.size() && std::isdigit(static_cast<unsigned char>(sql[p]))) ++p;
      }
      if (p < sql.size() && (sql[p] == 'e' || sql[p] == 'E')) {
        size_t save = p; ++p;
        if (p < sql.size() && (sql[p] == '+' || sql[p] == '-')) ++p;
        if (p < sql.size() && std::isdigit(static_cast<unsigned char>(sql[p]))) {
          while (p < sql.size() && std::isdigit(static_cast<unsigned char>(sql[p]))) ++p;
        } else p = save;
      }
      out.push_back({1, sql.substr(a, p - a)});
      continue;
    }
    if (c == '\'') {
      ++p;
      std::string s;
      while (p < sql.size()) {
        if (sql[p] == '\'') {
          if (p + 1 < sql.size() && sql[p + 1] == '\'') { s += '\''; p += 2; continue; }
          ++p; break;
        }
        if (sql[p] == '\\' && p + 1 < sql.size()) {
          char e = sql[p + 1];
          s += (e == 'n' ? '\n' : (e == 't' ? '\t' : (e == 'r' ? '\r' : e)));
          p += 2;
        } else s += sql[p++];
      }
      out.push_back({2, s});
      continue;
    }
    if (c == '`' ) {
      ++p; size_t a = p;
      while (p < sql.size() && sql[p] != '`') ++p;
      out.push_back({0, sql.substr(a, p - a)});
      if (p < sql.size()) ++p;
      continue;
    }
    if (p + 1 < sql.size() && ((sql[p] == '<' && sql[p + 1] == '=') ||
                               (sql[p] == '>' && sql[p + 1] == '=') ||
                               (sql[p] == '!' && sql[p + 1] == '=') ||
                               (sql[p] == '<' && sql[p + 1] == '>'))) {
      out.push_back({3, sql.substr(p, 2)});
      p += 2;
      continue;
    }
    out.push_back({3, std::string(1, static_cast<char>(c))});
    ++p;
  }
  out.push_back({4, ""});
  return out;
}

// ------------------------------------------------------------- SQL parser --

struct SqlParser {
  std::vector<Tok> t;
  size_t p = 0;
  explicit SqlParser(const std::string &sql) : t(lex(sql)) {}
  const Tok &cur() const { return t[p]; }
  bool at_end() const { return cur().t == 4; }
  bool kw(const char *k) const { return cur().t == 0 && ieq(cur().v, k); }
  bool punct(const char *k) const { return cur().t == 3 && cur().v == k; }
  bool eat_kw(const char *k) { if (kw(k)) { ++p; return true; } return false; }
  bool eat_punct(const char *k) { if (punct(k)) { ++p; return true; } return false; }
  std::string ident() { if (cur().t == 0) { std::string s = cur().v; ++p; return s; } throw DbError("syntax_error", "identifier expected"); }
  void expect_kw(const char *k) { if (!eat_kw(k)) throw DbError("syntax_error", std::string("expected ") + k); }
  void expect_punct(const char *k) { if (!eat_punct(k)) throw DbError("syntax_error", std::string("expected ") + k); }
};

static long long parse_i64(const std::string &s) {
  try { return std::stoll(s); }
  catch (...) { throw DbError("invalid_argument", "bad integer"); }
}

// Parse a vector literal, either from a string token ('[1,0]') or a bare [ ... ].
static std::vector<float> parse_vector_literal(SqlParser &ps, int dim) {
  std::string text;
  if (ps.cur().t == 2) { text = ps.cur().v; ++ps.p; }
  else if (ps.eat_punct("[")) {
    std::string body;
    int depth = 1;
    while (!ps.at_end() && depth > 0) {
      if (ps.punct("[")) ++depth;
      else if (ps.punct("]")) { --depth; if (!depth) break; }
      body += ps.cur().v;
      ++ps.p;
    }
    if (!ps.eat_punct("]")) throw DbError("invalid_vector", "unterminated vector");
    text = "[" + body + "]";
  } else throw DbError("invalid_vector", "vector literal expected");

  std::string inner = trim(text);
  if (inner.size() >= 2 && inner.front() == '[' && inner.back() == ']')
    inner = inner.substr(1, inner.size() - 2);
  else throw DbError("invalid_vector", "vector must be [ ... ]");
  if (trim(inner).empty() && dim == 0) return {};

  std::vector<float> out;
  const char *q = inner.c_str();
  while (*q) {
    while (*q && (std::isspace(static_cast<unsigned char>(*q)) || *q == ',')) ++q;
    if (!*q) break;
    // Reject alphabetic pseudo-numbers such as NaN / Infinity.
    if (std::isalpha(static_cast<unsigned char>(*q)) && *q != 'e' && *q != 'E')
      throw DbError("invalid_vector", "non-finite vector element");
    char *end = nullptr;
    double d = std::strtod(q, &end);
    if (end == q) throw DbError("invalid_vector", "bad vector element");
    q = end;
    if (!std::isfinite(d)) throw DbError("invalid_vector", "non-finite vector element");
    if (d > static_cast<double>(std::numeric_limits<float>::max()) ||
        d < -static_cast<double>(std::numeric_limits<float>::max()))
      throw DbError("vector_overflow", "float32 overflow");
    float f = static_cast<float>(d);
    if (!std::isfinite(f)) throw DbError("vector_overflow", "float32 overflow");
    out.push_back(f);
    while (*q && std::isspace(static_cast<unsigned char>(*q))) ++q;
    if (*q == ',') ++q;
    else if (*q) throw DbError("invalid_vector", "bad vector separator");
  }
  if (dim > 0 && static_cast<int>(out.size()) != dim)
    throw DbError("dimension_mismatch", "vector dimension mismatch");
  return out;
}

// ---------------------------------------------------------------- engine ---

struct SelectItem {
  int kind = 0;  // 0 column, 1 l2distance, 2 star
  int col = -1;
  std::string raw_name;   // column name / L2_DISTANCE column argument
  std::vector<float> query;
  std::string label;      // output column name (alias or expression text)
};

struct Cond {
  int col = -1;
  int op = 0;  // 0 =, 1 <, 2 <=, 3 >, 4 >=, 5 !=
  JV value;
};

struct OrderSpec {
  bool present = false;
  bool desc = false;
  int kind = 0;  // 1 column, 2 select item
  int ref = -1;
};

struct Reply {
  bool ok = true;
  JV columns;
  JV rows;
  long long affected = -1;
};

class Engine {
public:
  explicit Engine(std::string data_dir) : data_dir_(std::move(data_dir)) {}

  void recover() {
    std::string path = data_dir_ + "/wal.log";
    std::ifstream in(path, std::ios::binary);
    std::uint64_t valid = 0;
    if (in) {
      std::string line;
      while (std::getline(in, line)) {
        // std::getline sets eofbit on a final line that had no newline: torn tail.
        bool terminated = !in.eof();
        if (!terminated) break;  // incomplete tail
        size_t tab = line.find('\t');
        if (tab == std::string::npos) break;
        std::string crc = line.substr(0, tab);
        std::string payload = line.substr(tab + 1);
        if (crc != std::to_string(fnv1a(payload))) break;
        JParser jp(payload);
        JV root;
        if (!jp.value(root) || root.type != JV::OBJ) break;
        const JV *ops = root.find("ops");
        if (!ops || ops->type != JV::ARR) break;
        try { apply_ops_json(*ops); }
        catch (...) { break; }
        valid += line.size() + 1;
      }
    }
    in.close();
    if (valid > 0 || file_exists(path)) {
      // Truncate any torn tail so subsequent appends stay well-formed.
      std::uint64_t size = file_size(path);
      if (size > valid) platform_truncate(path, valid);
    }
    wal_ = std::fopen(path.c_str(), "ab");
    if (!wal_) throw DbError("storage_error", "cannot open WAL");
  }

  void close() {
    // Uncommitted work is deliberately dropped (shutdown rolls it back).
    txn_ = Txn();
    if (wal_) { std::fflush(wal_); std::fclose(wal_); wal_ = nullptr; }
  }

  Reply execute(const std::string &sql, const std::string &mode) {
    SqlParser ps(sql);
    if (ps.at_end()) throw DbError("syntax_error", "empty statement");
    if (ps.kw("CREATE")) return exec_create(ps);
    if (ps.kw("INSERT")) return exec_insert(ps);
    if (ps.kw("SELECT")) return exec_select(ps, mode);
    if (ps.kw("UPDATE")) return exec_update(ps);
    if (ps.kw("DELETE")) return exec_delete(ps);
    if (ps.eat_kw("BEGIN")) { finish(ps); if (txn_.active) throw DbError("nested_transaction", "transaction already active"); txn_ = Txn(); txn_.active = true; return {}; }
    if (ps.eat_kw("START")) { ps.expect_kw("TRANSACTION"); finish(ps); if (txn_.active) throw DbError("nested_transaction", "transaction already active"); txn_ = Txn(); txn_.active = true; return {}; }
    if (ps.eat_kw("COMMIT")) { finish(ps); if (!txn_.active) throw DbError("no_transaction", "no active transaction"); commit(); txn_ = Txn(); return {}; }
    if (ps.eat_kw("ROLLBACK")) { finish(ps); if (!txn_.active) throw DbError("no_transaction", "no active transaction"); txn_ = Txn(); return {}; }
    throw DbError("unsupported_sql", "statement is not supported");
  }

private:
  std::string data_dir_;
  FILE *wal_ = nullptr;
  std::map<std::string, Table> tables_;
  Txn txn_;
  long long seq_ = 0;

  static bool file_exists(const std::string &p) { std::ifstream f(p, std::ios::binary); return f.good(); }
  static std::uint64_t file_size(const std::string &p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    return f ? static_cast<std::uint64_t>(f.tellg()) : 0;
  }
  static void finish(SqlParser &ps) {
    while (ps.eat_punct(";")) {}
    if (!ps.at_end()) throw DbError("syntax_error", "trailing tokens");
  }

  Table *find_table(const std::string &name) {
    auto nt = txn_.new_tables.find(name);
    if (nt != txn_.new_tables.end()) return &nt->second;
    auto it = tables_.find(name);
    return it == tables_.end() ? nullptr : &it->second;
  }
  const Table *find_table(const std::string &name) const {
    auto nt = txn_.new_tables.find(name);
    if (nt != txn_.new_tables.end()) return &nt->second;
    auto it = tables_.find(name);
    return it == tables_.end() ? nullptr : &it->second;
  }

  std::vector<ViewRow> view(const Table &t) const {
    std::vector<ViewRow> out;
    auto ch = txn_.changes.find(t.name);
    if (ch == txn_.changes.end()) {
      out.reserve(t.rows.size());
      for (const auto &kv : t.rows) out.push_back({kv.first, &kv.second});
      return out;
    }
    for (const auto &kv : t.rows) {
      auto it = ch->second.find(kv.first);
      if (it == ch->second.end()) out.push_back({kv.first, &kv.second});
      else if (it->second) out.push_back({kv.first, &*it->second});
    }
    for (const auto &kv : ch->second) {
      if (kv.second && !t.rows.count(kv.first)) out.push_back({kv.first, &*kv.second});
    }
    return out;
  }

  bool visible_id(const Table &t, long long id, const std::set<long long> &pending) const {
    if (pending.count(id)) return true;
    auto ch = txn_.changes.find(t.name);
    if (ch != txn_.changes.end()) {
      auto it = ch->second.find(id);
      if (it != ch->second.end()) return static_cast<bool>(it->second);
    }
    return t.rows.count(id) != 0;
  }

  void apply_op_to_base(const Op &op) {
    if (op.kind == 0) {
      Table t;
      t.name = op.table;
      t.cols = op.cols;
      for (size_t i = 0; i < t.cols.size(); ++i) {
        if (t.cols[i].pk && t.cols[i].type == T_BIGINT) t.pk_idx = static_cast<int>(i);
        if (t.cols[i].type == T_VECTOR) t.vec_idx = static_cast<int>(i);
      }
      tables_[op.table] = std::move(t);
    } else if (op.kind == 1) {
      auto it = tables_.find(op.table);
      if (it != tables_.end()) it->second.rows[op.id] = op.row;
    } else if (op.kind == 2) {
      auto it = tables_.find(op.table);
      if (it != tables_.end()) it->second.rows.erase(op.id);
    } else if (op.kind == 3) {
      auto it = tables_.find(op.table);
      if (it != tables_.end()) it->second.has_index = true;
    }
  }

  void apply_ops_json(const JV &ops) {
    for (const auto &e : ops.arr) {
      if (e.type != JV::ARR || e.arr.size() < 2) throw DbError("storage_error", "bad WAL op");
      int kind = static_cast<int>(e.arr[0].i);
      Op op;
      op.kind = kind;
      op.table = e.arr[1].s;
      if (kind == 0) {
        if (e.arr.size() < 3 || e.arr[2].type != JV::ARR) throw DbError("storage_error", "bad create");
        for (const auto &c : e.arr[2].arr) {
          Column col;
          col.name = c.arr[0].s;
          col.type = static_cast<int>(c.arr[1].i);
          col.len = static_cast<int>(c.arr[2].i);
          col.pk = c.arr[3].i != 0;
          op.cols.push_back(std::move(col));
        }
      } else if (kind == 1) {
        op.id = e.arr[2].i;
        Table t;
        t.cols = {};
        auto it = tables_.find(op.table);
        // create_table always precedes puts for that table in the WAL.
        if (it == tables_.end()) throw DbError("storage_error", "put before create");
        const auto &cols = it->second.cols;
        const JV &row = e.arr[3];
        for (size_t i = 0; i < cols.size(); ++i) {
          Cell cell;
          const JV &v = row.arr[i];
          if (cols[i].type == T_BIGINT) {
            cell.i = v.type == JV::DBL ? static_cast<long long>(v.d) : v.i;
          } else if (cols[i].type == T_VARCHAR) {
            cell.s = v.s;
          } else {
            for (const auto &f : v.arr) {
              double val = f.type == JV::DBL ? f.d :
                           (f.type == JV::INT ? static_cast<double>(f.i) : 0.0);
              cell.v.push_back(static_cast<float>(val));
            }
          }
          op.row.push_back(std::move(cell));
        }
      } else if (kind == 2) {
        op.id = e.arr[2].i;
      }
      apply_op_to_base(op);
    }
  }

  void commit_ops(std::vector<Op> ops) {
    if (ops.empty()) return;
    std::string payload;
    // Only column definitions are needed to serialise a row.  Copying whole
    // Table objects here would make every batch O(total rows) and inflate the
    // peak-memory metric for no reason.
    {
      std::map<std::string, std::vector<Column>> defs;
      for (const auto &kv : tables_) defs[kv.first] = kv.second.cols;
      for (const auto &kv : txn_.new_tables) defs[kv.first] = kv.second.cols;
      payload = std::string("{\"ops\":") + ops_to_json(ops, defs) + "}";
    }
    std::string line = std::to_string(fnv1a(payload)) + "\t" + payload + "\n";
    if (std::fwrite(line.data(), 1, line.size(), wal_) != line.size())
      throw DbError("storage_error", "WAL write failed");
    platform_fsync(wal_);
    ++seq_;
    for (const auto &op : ops) apply_op_to_base(op);
  }

  void stage(const std::vector<Op> &stmt_ops) {
    if (stmt_ops.empty()) return;
    if (!txn_.active) { commit_ops(stmt_ops); return; }
    for (const auto &op : stmt_ops) {
      txn_.ops.push_back(op);
      if (op.kind == 0) {
        Table t;
        t.name = op.table; t.cols = op.cols;
        for (size_t i = 0; i < t.cols.size(); ++i) {
          if (t.cols[i].pk) t.pk_idx = static_cast<int>(i);
          if (t.cols[i].type == T_VECTOR) t.vec_idx = static_cast<int>(i);
        }
        txn_.new_tables[op.table] = std::move(t);
      } else if (op.kind == 1) {
        txn_.changes[op.table][op.id] = op.row;
      } else if (op.kind == 2) {
        txn_.changes[op.table][op.id] = std::nullopt;
      } else if (op.kind == 3) {
        Table *t = find_table(op.table);
        if (t) t->has_index = true;
      }
    }
  }

  void commit() {
    if (!txn_.ops.empty()) commit_ops(txn_.ops);
    // new_tables / changes become part of the base through apply_op_to_base.
  }

  // ------------------------------------------------------------- CREATE ----

  Reply exec_create(SqlParser &ps) {
    ps.expect_kw("CREATE");
    if (ps.eat_kw("VECTOR")) {
      ps.expect_kw("INDEX");
      std::string idx = ps.ident();
      ps.expect_kw("ON");
      std::string table = ps.ident();
      ps.expect_punct("(");
      std::string col = ps.ident();
      ps.expect_punct(")");
      finish(ps);
      Table *t = find_table(table);
      if (!t) throw DbError("table_not_found", "unknown table");
      if (t->vec_idx < 0 || t->col_index(col) != t->vec_idx)
        throw DbError("invalid_argument", "vector column required");
      std::vector<Op> ops;
      Op op; op.kind = 3; op.table = table;
      ops.push_back(op);
      stage(ops);
      (void)idx;
      return {};
    }
    if (!ps.eat_kw("TABLE")) throw DbError("unsupported_sql", "unsupported CREATE form");
    std::string name = ps.ident();
    ps.expect_punct("(");
    std::vector<Column> cols;
    do {
      Column c;
      c.name = ps.ident();
      if (ps.eat_kw("BIGINT")) c.type = T_BIGINT;
      else if (ps.eat_kw("VARCHAR")) {
        c.type = T_VARCHAR;
        ps.expect_punct("(");
        c.len = static_cast<int>(parse_i64(ps.cur().v)); ++ps.p;
        ps.expect_punct(")");
      } else if (ps.eat_kw("VECTOR")) {
        c.type = T_VECTOR;
        ps.expect_punct("(");
        c.len = static_cast<int>(parse_i64(ps.cur().v)); ++ps.p;
        ps.expect_punct(")");
      } else throw DbError("unsupported_sql", "unsupported column type");
      if (ps.eat_kw("PRIMARY")) { ps.expect_kw("KEY"); c.pk = true; }
      cols.push_back(std::move(c));
    } while (ps.eat_punct(","));
    ps.expect_punct(")");
    finish(ps);
    if (find_table(name)) throw DbError("table_exists", "table already exists");
    int pks = 0, vecs = 0;
    for (const auto &c : cols) {
      if (c.pk) { ++pks; if (c.type != T_BIGINT) throw DbError("invalid_argument", "primary key must be BIGINT"); }
      if (c.type == T_VECTOR) ++vecs;
      if (c.type == T_VARCHAR && c.len <= 0) throw DbError("invalid_argument", "bad VARCHAR length");
      if (c.type == T_VECTOR && c.len <= 0) throw DbError("invalid_argument", "bad vector dimension");
    }
    if (pks != 1) throw DbError("invalid_argument", "exactly one BIGINT primary key required");
    if (vecs > 1) throw DbError("invalid_argument", "at most one vector column");
    std::vector<Op> ops;
    Op op; op.kind = 0; op.table = name; op.cols = cols;
    ops.push_back(op);
    stage(ops);
    return {};
  }

  // ------------------------------------------------------------- values ----

  Cell parse_value_for(const Column &col, SqlParser &ps) {
    Cell cell;
    if (col.type == T_BIGINT) {
      bool neg = false;
      if (ps.eat_punct("-")) neg = true;
      if (ps.cur().t != 1) throw DbError("type_mismatch", "integer expected");
      long long v = parse_i64(ps.cur().v); ++ps.p;
      cell.i = neg ? -v : v;
    } else if (col.type == T_VARCHAR) {
      if (ps.cur().t != 2) throw DbError("type_mismatch", "string expected");
      cell.s = ps.cur().v; ++ps.p;
      if (static_cast<int>(utf8_chars(cell.s)) > col.len)
        throw DbError("value_too_long", "VARCHAR length exceeded");
    } else {
      cell.v = parse_vector_literal(ps, col.len);
    }
    return cell;
  }

  static JV cell_json(const Cell &c, int type) { return cell_to_json(c, type); }

  // ------------------------------------------------------------- INSERT ----

  Reply exec_insert(SqlParser &ps) {
    ps.expect_kw("INSERT");
    ps.expect_kw("INTO");
    std::string name = ps.ident();
    Table *t = find_table(name);
    if (!t) throw DbError("table_not_found", "unknown table");
    std::vector<int> positions;
    if (ps.eat_punct("(")) {
      do {
        std::string c = ps.ident();
        int idx = t->col_index(c);
        if (idx < 0) throw DbError("column_not_found", "unknown column");
        positions.push_back(idx);
      } while (ps.eat_punct(","));
      ps.expect_punct(")");
    } else {
      for (size_t i = 0; i < t->cols.size(); ++i) positions.push_back(static_cast<int>(i));
    }
    ps.expect_kw("VALUES");
    std::vector<Row> pending_rows;
    std::set<long long> pending_ids;
    do {
      ps.expect_punct("(");
      Row row(static_cast<size_t>(t->cols.size()));
      if (ps.punct(")")) throw DbError("syntax_error", "empty VALUES tuple");
      for (size_t k = 0; k < positions.size(); ++k) {
        if (k) ps.expect_punct(",");
        int idx = positions[k];
        row[idx] = parse_value_for(t->cols[idx], ps);
      }
      ps.expect_punct(")");
      long long id = row[t->pk_idx].i;
      if (visible_id(*t, id, pending_ids))
        throw DbError("duplicate_key", "duplicate primary key");
      pending_ids.insert(id);
      pending_rows.push_back(std::move(row));
    } while (ps.eat_punct(","));
    finish(ps);
    std::vector<Op> ops;
    for (auto &r : pending_rows) {
      Op op; op.kind = 1; op.table = name; op.id = r[t->pk_idx].i; op.row = std::move(r);
      ops.push_back(std::move(op));
    }
    stage(ops);
    Reply rep; rep.affected = static_cast<long long>(ops.size());
    return rep;
  }

  // ------------------------------------------------------------- WHERE -----

  static int parse_cmp(SqlParser &ps) {
    if (ps.eat_punct("=")) return 0;
    if (ps.eat_punct("<")) return 1;
    if (ps.eat_punct("<=")) return 2;
    if (ps.eat_punct(">")) return 3;
    if (ps.eat_punct(">=")) return 4;
    if (ps.punct("!=") || ps.punct("<>")) { ++ps.p; return 5; }
    throw DbError("syntax_error", "comparison operator expected");
  }

  std::vector<Cond> parse_where(SqlParser &ps, const Table &t) {
    std::vector<Cond> out;
    if (!ps.eat_kw("WHERE")) return out;
    do {
      Cond c;
      std::string col = ps.ident();
      c.col = t.col_index(col);
      if (c.col < 0) throw DbError("column_not_found", "unknown column");
      c.op = parse_cmp(ps);
      bool neg = false;
      if (ps.eat_punct("-")) neg = true;
      if (ps.cur().t == 2) c.value = JV::strv(ps.cur().v);
      else if (ps.cur().t == 1) c.value = JV::intv(neg ? -parse_i64(ps.cur().v) : parse_i64(ps.cur().v));
      else throw DbError("syntax_error", "value expected");
      ++ps.p;
      out.push_back(std::move(c));
    } while (ps.eat_kw("AND"));
    return out;
  }

  bool matches(const Row &r, const Table &t, const std::vector<Cond> &conds) const {
    for (const auto &c : conds) {
      int cmp = 0;
      if (t.cols[c.col].type == T_BIGINT) {
        long long a = r[c.col].i;
        long long b = c.value.type == JV::INT ? c.value.i :
                      (c.value.type == JV::STR ? parse_i64(c.value.s) : static_cast<long long>(c.value.d));
        cmp = a < b ? -1 : (a > b ? 1 : 0);
      } else {
        std::string a = r[c.col].s;
        std::string b = c.value.type == JV::STR ? c.value.s :
                        (c.value.type == JV::INT ? std::to_string(c.value.i) : dump_double(c.value.d));
        cmp = a < b ? -1 : (a > b ? 1 : 0);
      }
      bool pass = false;
      switch (c.op) {
        case 0: pass = cmp == 0; break;
        case 1: pass = cmp < 0; break;
        case 2: pass = cmp <= 0; break;
        case 3: pass = cmp > 0; break;
        case 4: pass = cmp >= 0; break;
        case 5: pass = cmp != 0; break;
      }
      if (!pass) return false;
    }
    return true;
  }

  // ------------------------------------------------------------- SELECT ----

  Reply exec_select(SqlParser &ps, const std::string &mode) {
    ps.expect_kw("SELECT");
    std::vector<SelectItem> items;
    if (ps.eat_punct("*")) {
      SelectItem it; it.kind = 2; items.push_back(it);
    } else {
      do {
        SelectItem it;
        if (ps.eat_kw("L2_DISTANCE")) {
          ps.expect_punct("(");
          it.raw_name = ps.ident();
          ps.expect_punct(",");
          it.query = parse_vector_literal(ps, 0);
          ps.expect_punct(")");
          it.kind = 1;
          it.label = "L2_DISTANCE(" + it.raw_name + ", ?)";
          if (ps.eat_kw("AS")) it.label = ps.ident();
          items.push_back(std::move(it));
          continue;
        }
        it.kind = 0;
        it.raw_name = ps.ident();
        it.label = it.raw_name;
        if (ps.eat_kw("AS")) it.label = ps.ident();
        items.push_back(std::move(it));
      } while (ps.eat_punct(","));
    }
    ps.expect_kw("FROM");
    std::string name = ps.ident();
    Table *t = find_table(name);
    if (!t) throw DbError("table_not_found", "unknown table");

    // Resolve projected columns now that the table definition is known.
    for (auto &it : items) {
      if (it.kind == 0) {
        it.col = t->col_index(it.raw_name);
        if (it.col < 0) throw DbError("column_not_found", "unknown column " + it.raw_name);
      } else if (it.kind == 1) {
        it.col = t->col_index(it.raw_name);
        if (it.col < 0 || t->cols[it.col].type != T_VECTOR)
          throw DbError("invalid_argument", "L2_DISTANCE requires a vector column");
        if (static_cast<int>(it.query.size()) != t->cols[it.col].len)
          throw DbError("dimension_mismatch", "query vector dimension mismatch");
      }
    }
    std::vector<Cond> conds = parse_where(ps, *t);
    OrderSpec order;
    if (ps.eat_kw("ORDER")) {
      ps.expect_kw("BY");
      order.present = true;
      if (ps.eat_kw("L2_DISTANCE")) {
        ps.expect_punct("(");
        ps.ident(); ps.expect_punct(",");
        parse_vector_literal(ps, 0);
        ps.expect_punct(")");
        order.kind = 2; order.ref = -1;
        for (size_t i = 0; i < items.size(); ++i) if (items[i].kind == 1) { order.ref = static_cast<int>(i); break; }
      } else {
        std::string ref = ps.ident();
        bool found = false;
        // alias of a computed distance -> sort by that expression's value
        for (size_t i = 0; i < items.size(); ++i) {
          if (items[i].label == ref && items[i].kind == 1) {
            order.kind = 2; order.ref = static_cast<int>(i); found = true; break;
          }
        }
        // alias of a plain column -> sort by the underlying column value
        if (!found) {
          for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].label == ref && items[i].kind == 0) {
              order.kind = 1; order.ref = items[i].col; found = true; break;
            }
          }
        }
        if (!found) {
          int ci = t->col_index(ref);
          if (ci < 0) throw DbError("column_not_found", "unknown ORDER BY expression");
          order.kind = 1; order.ref = ci;
        }
      }
      if (ps.eat_kw("DESC")) order.desc = true;
      else if (ps.eat_kw("ASC")) order.desc = false;
    }
    long long limit = -1;
    if (ps.eat_kw("LIMIT")) {
      if (ps.cur().t != 1) throw DbError("syntax_error", "LIMIT expects integer");
      limit = parse_i64(ps.cur().v); ++ps.p;
      if (limit < 0) throw DbError("invalid_argument", "negative LIMIT");
    }
    finish(ps);
    (void)mode;  // ann mode uses the same exact scan -> recall is always 1.0

    std::vector<ViewRow> rows = view(*t);
    // Phase 1 only computes a lightweight sort key per candidate row.  JSON
    // projection is deferred until after TopK truncation, so a 10k-row scan
    // does not allocate 10k result objects (query time dominates the score).
    struct Cand {
      long long pk;
      const Row *row;
      bool sort_num;
      double num;
      std::string text;
    };
    auto distance_of = [](const Row &r, int col, const std::vector<float> &query) {
      double sum = 0.0;
      const auto &v = r[col].v;
      for (size_t k = 0; k < v.size(); ++k) {
        double diff = static_cast<double>(v[k]) - static_cast<double>(query[k]);
        sum += diff * diff;
      }
      return std::sqrt(sum);
    };
    std::vector<Cand> cand;
    cand.reserve(rows.size());
    for (const auto &vr : rows) {
      if (!matches(*vr.row, *t, conds)) continue;
      Cand c{vr.id, vr.row, true, 0.0, std::string()};
      if (order.present) {
        if (order.kind == 2 && order.ref >= 0 && items[order.ref].kind == 1) {
          c.num = distance_of(*vr.row, items[order.ref].col, items[order.ref].query);
        } else if (order.kind == 1) {
          const Column &col = t->cols[order.ref];
          if (col.type == T_BIGINT) c.num = static_cast<double>(vr.row->at(order.ref).i);
          else if (col.type == T_VARCHAR) { c.sort_num = false; c.text = vr.row->at(order.ref).s; }
          else { c.sort_num = false; }
        }
      }
      cand.push_back(std::move(c));
    }

    std::sort(cand.begin(), cand.end(), [&](const Cand &a, const Cand &b) {
      if (order.present) {
        int cmp;
        if (a.sort_num && b.sort_num) cmp = a.num < b.num ? -1 : (a.num > b.num ? 1 : 0);
        else cmp = a.text < b.text ? -1 : (a.text > b.text ? 1 : 0);
        if (cmp) return order.desc ? cmp > 0 : cmp < 0;
      }
      return a.pk < b.pk;  // stable tie-break: primary key ascending
    });
    if (limit >= 0 && static_cast<size_t>(limit) < cand.size()) cand.resize(static_cast<size_t>(limit));

    JV cols = JV::arrv();
    for (const auto &it : items) {
      if (it.kind == 2) for (const auto &c : t->cols) cols.arr.push_back(JV::strv(c.name));
      else cols.arr.push_back(JV::strv(it.label));
    }
    JV out = JV::arrv();
    for (const auto &c : cand) {
      JV row = JV::arrv();
      for (const auto &it : items) {
        if (it.kind == 2) {
          for (size_t k = 0; k < t->cols.size(); ++k) row.arr.push_back(cell_json(c.row->at(k), t->cols[k].type));
        } else if (it.kind == 0) {
          row.arr.push_back(cell_json(c.row->at(it.col), t->cols[it.col].type));
        } else {
          row.arr.push_back(JV::dblv(distance_of(*c.row, it.col, it.query)));
        }
      }
      out.arr.push_back(std::move(row));
    }
    Reply rep; rep.columns = std::move(cols); rep.rows = std::move(out);
    return rep;
  }

  // ------------------------------------------------------------- UPDATE ----

  Reply exec_update(SqlParser &ps) {
    ps.expect_kw("UPDATE");
    std::string name = ps.ident();
    Table *t = find_table(name);
    if (!t) throw DbError("table_not_found", "unknown table");
    ps.expect_kw("SET");
    std::vector<std::pair<int, Cell>> assigns;
    do {
      std::string col = ps.ident();
      int idx = t->col_index(col);
      if (idx < 0) throw DbError("column_not_found", "unknown column");
      ps.expect_punct("=");
      assigns.emplace_back(idx, parse_value_for(t->cols[idx], ps));
    } while (ps.eat_punct(","));
    std::vector<Cond> conds = parse_where(ps, *t);
    finish(ps);
    for (const auto &a : assigns) if (a.first == t->pk_idx) throw DbError("invalid_argument", "primary key update not supported");
    std::vector<ViewRow> rows = view(*t);
    std::vector<Op> ops;
    for (const auto &vr : rows) {
      if (!matches(*vr.row, *t, conds)) continue;
      Row nr = *vr.row;
      for (const auto &a : assigns) nr[a.first] = a.second;
      Op op; op.kind = 1; op.table = name; op.id = vr.id; op.row = std::move(nr);
      ops.push_back(std::move(op));
    }
    stage(ops);
    Reply rep; rep.affected = static_cast<long long>(ops.size());
    return rep;
  }

  // ------------------------------------------------------------- DELETE ----

  Reply exec_delete(SqlParser &ps) {
    ps.expect_kw("DELETE");
    ps.expect_kw("FROM");
    std::string name = ps.ident();
    Table *t = find_table(name);
    if (!t) throw DbError("table_not_found", "unknown table");
    std::vector<Cond> conds = parse_where(ps, *t);
    finish(ps);
    std::vector<ViewRow> rows = view(*t);
    std::vector<Op> ops;
    for (const auto &vr : rows) {
      if (!matches(*vr.row, *t, conds)) continue;
      Op op; op.kind = 2; op.table = name; op.id = vr.id;
      ops.push_back(op);
    }
    stage(ops);
    Reply rep; rep.affected = static_cast<long long>(ops.size());
    return rep;
  }
};

// ------------------------------------------------------------------ main ---

static std::string data_dir_from_args(int argc, char **argv) {
  std::string dir;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--data-dir" && i + 1 < argc) dir = argv[++i];
    else if (a.rfind("--data-dir=", 0) == 0) dir = a.substr(11);
  }
  if (dir.empty()) {
    const char *env = std::getenv("VECTORDB_DATA_DIR");
    if (env && *env) dir = env;
  }
  if (dir.empty()) dir = "/data";
  return dir;
}

int main(int argc, char **argv) {
  platform_binary_stdout();
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);
  std::string dir = data_dir_from_args(argc, argv);
#ifdef _WIN32
  _mkdir(dir.c_str());
#else
  mkdir(dir.c_str(), 0777);
#endif

  Engine engine(dir);
  try { engine.recover(); }
  catch (const DbError &e) {
    std::cerr << "recovery failed: " << e.code << " " << e.message << std::endl;
    return 1;
  }
  std::cout << "{\"type\":\"ready\"}\n";
  std::cout.flush();

  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    JParser jp(line);
    JV req;
    if (!jp.value(req) || req.type != JV::OBJ) {
      std::cerr << "invalid request JSON" << std::endl;
      continue;
    }
    const JV *idv = req.find("id");
    JV id = idv ? *idv : JV::nullv();
    const JV *opv = req.find("op");
    std::string op = opv && opv->type == JV::STR ? opv->s : "";
    std::string out;
    if (op == "shutdown") {
      engine.close();
      out = "{\"id\":" + jdump(id) + ",\"ok\":true}\n";
      std::cout << out;
      std::cout.flush();
      return 0;
    }
    if (op != "execute") {
      out = "{\"id\":" + jdump(id) + ",\"ok\":false,\"error\":{\"code\":\"unsupported_op\",\"message\":\"unsupported operation\"}}\n";
      std::cout << out; std::cout.flush();
      continue;
    }
    const JV *sqlv = req.find("sql");
    const JV *modev = req.find("search_mode");
    std::string sql = sqlv && sqlv->type == JV::STR ? sqlv->s : "";
    std::string mode = modev && modev->type == JV::STR ? modev->s : "exact";
    try {
      Reply rep = engine.execute(sql, mode);
      out = "{\"id\":" + jdump(id) + ",\"ok\":true";
      if (rep.affected >= 0) out += ",\"affected_rows\":" + std::to_string(rep.affected);
      else if (rep.columns.type == JV::ARR) out += ",\"columns\":" + jdump(rep.columns) + ",\"rows\":" + jdump(rep.rows);
      out += "}\n";
    } catch (const DbError &e) {
      JV err = JV::objv();
      err.obj.emplace_back("code", JV::strv(e.code));
      err.obj.emplace_back("message", JV::strv(e.message));
      out = "{\"id\":" + jdump(id) + ",\"ok\":false,\"error\":" + jdump(err) + "}\n";
    } catch (const std::exception &e) {
      JV err = JV::objv();
      err.obj.emplace_back("code", JV::strv("internal_error"));
      err.obj.emplace_back("message", JV::strv(e.what()));
      out = "{\"id\":" + jdump(id) + ",\"ok\":false,\"error\":" + jdump(err) + "}\n";
    } catch (...) {
      out = "{\"id\":" + jdump(id) + ",\"ok\":false,\"error\":{\"code\":\"internal_error\",\"message\":\"unknown failure\"}}\n";
    }
    std::cout << out;
    std::cout.flush();
  }
  engine.close();
  return 0;
}
