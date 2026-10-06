#pragma once
// Minimal deterministic host platform for testing the actual firmware engines.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <sstream>
#include <iomanip>
#include <type_traits>
using std::isfinite;
inline uint32_t testNow = 0;
inline uint32_t millis() { return testNow; }
class String {
 public:
  std::string value;
  String() = default;
  String(const char *s) : value(s ? s : "") {}
  String(const std::string &s) : value(s) {}
  String(char c) : value(1, c) {}
  template<typename T, typename = std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, char>>>
  String(T n) : value(std::to_string(n)) {}
  String(double n, unsigned digits = 2) { std::ostringstream s; s << std::fixed << std::setprecision(digits) << n; value = s.str(); }
  const char *c_str() const { return value.c_str(); }
  size_t length() const { return value.size(); }
  void reserve(size_t n) { value.reserve(n); }
  char operator[](size_t n) const { return value[n]; }
  String substring(unsigned start, unsigned end) const { start = std::min<size_t>(start, value.size()); end = std::min<size_t>(end, value.size()); return value.substr(start, end >= start ? end - start : 0); }
  String substring(unsigned start) const { return substring(start, static_cast<unsigned>(value.size())); }
  int indexOf(char c, unsigned start = 0) const { auto i = value.find(c, start); return i == std::string::npos ? -1 : static_cast<int>(i); }
  bool startsWith(const String &s) const { return value.rfind(s.value, 0) == 0; }
  bool equalsIgnoreCase(const String &s) const { if (length() != s.length()) return false; for (size_t i=0;i<length();++i) if (std::tolower(static_cast<unsigned char>(value[i])) != std::tolower(static_cast<unsigned char>(s[i]))) return false; return true; }
  void trim() { auto first=value.find_first_not_of(" \r\n\t"); auto last=value.find_last_not_of(" \r\n\t"); value=first==std::string::npos ? "" : value.substr(first,last-first+1); }
  void toCharArray(char *dest, size_t size) const { if (size) { size_t n=std::min(size-1,length()); memcpy(dest,c_str(),n); dest[n]=0; } }
  String &operator+=(const String &s) { value += s.value; return *this; }
  friend String operator+(const String &a,const String &b) { return a.value+b.value; }
  friend bool operator==(const String &a,const String &b) { return a.value==b.value; }
  friend bool operator!=(const String &a,const String &b) { return !(a==b); }
};
