#pragma once
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace oceanbase { namespace observer { namespace ai_column_value {
// These routines also run in the standalone native regression harness. They
// validate the real worker's values, not a copied test implementation.
inline bool integer(const std::string &s, int64_t &value)
{
  if (s.empty()) return false;
  const char *begin = s.data(), *end = begin + s.size();
  if (*begin == '+') { if (++begin == end || *begin == '-' || *begin == '+') return false; }
  auto r = std::from_chars(begin, end, value, 10);
  return r.ec == std::errc() && r.ptr == end;
}
inline bool utf8_length(const std::string &s, size_t &count)
{
  count = 0;
  for (size_t i = 0; i < s.size(); ++count) {
    unsigned c = static_cast<unsigned char>(s[i++]);
    if (c < 128) continue;
    unsigned n = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 99;
    if (n == 99 || i + n > s.size()) return false;
    unsigned code = c & ((1u << (6 - n)) - 1);
    for (unsigned j = 0; j < n; ++j) {
      unsigned b = static_cast<unsigned char>(s[i++]);
      if ((b & 0xc0) != 0x80) return false;
      code = (code << 6) | (b & 63);
    }
    if (code < (n == 1 ? 128u : n == 2 ? 2048u : 65536u) || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
  }
  return true;
}
inline void whitespace(const std::string &s, size_t &p)
{ while (p < s.size() && (s[p] == ' ' || s[p] == '\n' || s[p] == '\r' || s[p] == '\t')) ++p; }
inline bool vector(const std::string &s, int64_t dimension)
{
  if (dimension <= 0 || dimension > 65536 || s.size() > 4 * 1024 * 1024) return false;
  size_t p = 0; whitespace(s, p);
  if (p == s.size() || s[p++] != '[') return false;
  int64_t count = 0;
  for (;;) {
    whitespace(s, p);
    size_t begin = p;
    if (p < s.size() && s[p] == '-') ++p;
    if (p == s.size() || s[p] < '0' || s[p] > '9') return false;
    if (s[p] == '0') ++p;
    else while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
    if (p < s.size() && s[p] == '.') {
      size_t b = ++p;
      while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
      if (b == p) return false;
    }
    if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
      ++p;
      if (p < s.size() && (s[p] == '+' || s[p] == '-')) ++p;
      size_t b = p;
      while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
      if (b == p) return false;
    }
    const std::string number = s.substr(begin, p - begin);
    char *end = nullptr; errno = 0;
    double v = std::strtod(number.c_str(), &end);
    if (errno == ERANGE || end != number.c_str() + number.size() || !std::isfinite(v)
        || !std::isfinite(static_cast<float>(v)) || ++count > dimension) return false;
    whitespace(s, p);
    if (p == s.size()) return false;
    if (s[p] == ']') { ++p; whitespace(s, p); return p == s.size() && count == dimension; }
    if (s[p++] != ',') return false;
  }
}
inline bool membership(const std::string &s, std::vector<int64_t> &ids)
{
  size_t p = 0; whitespace(s, p);
  if (p == s.size() || s[p++] != '[') return false;
  for (;;) {
    whitespace(s, p); size_t b = p;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
    int64_t id = 0;
    if (p - b > 1 && s[b] == '0') return false;
    if (!integer(s.substr(b, p - b), id) || id <= 0 || ids.size() >= 4096) return false;
    for (auto existing : ids) if (existing == id) return false;
    ids.push_back(id); whitespace(s, p);
    if (p == s.size()) return false;
    if (s[p] == ']') { ++p; whitespace(s, p); return p == s.size(); }
    if (s[p++] != ',') return false;
  }
}
inline int64_t backoff_us(int64_t retries)
{ return 1000000LL << (retries < 1 ? 0 : retries > 6 ? 5 : retries - 1); }
}}}
