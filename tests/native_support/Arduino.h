#pragma once
#include <string>
#include <type_traits>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <iostream>
using std::min;
using std::max;
class String {
 public:
  std::string value;
  String() = default;
  String(const char* s): value(s ? s : "") {}
  String(std::string s): value(std::move(s)) {}
  template<class T, typename std::enable_if<std::is_arithmetic<T>::value, int>::type = 0>
  String(T n): value(std::to_string(n)) {}
  const char* c_str() const { return value.c_str(); }
  size_t length() const { return value.size(); }
  bool isEmpty() const { return value.empty(); }
  bool startsWith(const char* prefix) const { return value.rfind(prefix, 0) == 0; }
  int indexOf(char c, int start = 0) const { auto n = value.find(c, start); return n == std::string::npos ? -1 : int(n); }
  int indexOf(const char* s) const { auto n = value.find(s); return n == std::string::npos ? -1 : int(n); }
  String substring(int start) const { return value.substr(start); }
  String substring(int start, int end) const { return value.substr(start, end-start); }
  String& operator+=(const String& b) { value += b.value; return *this; }
  String& operator+=(char c) { value += c; return *this; }
  friend String operator+(const String& a, const String& b) { return a.value+b.value; }
  friend bool operator==(const String& a, const String& b) { return a.value==b.value; }
  friend bool operator!=(const String& a, const String& b) { return !(a==b); }
};
struct SerialMock { template<class... T> void printf(T...) {} void println(const char*) {} };
inline SerialMock Serial;
inline uint32_t fakeMillis = 0;
inline unsigned long millis() { return fakeMillis; }
struct EspMock { uint64_t getEfuseMac() { return 123; } };
inline EspMock ESP;
