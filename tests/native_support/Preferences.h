#pragma once
#include <map>
#include <vector>
#include "Arduino.h"
class Preferences {
 public:
  inline static std::map<std::string,std::vector<uint8_t>> data;
  bool begin(const char*, bool) { return true; }
  void end() {}
  bool isKey(const char* k) { return data.count(k); }
  size_t getBytesLength(const char* k) { return data[k].size(); }
  size_t getBytes(const char* k, void* out, size_t n) { auto it=data.find(k); if(it==data.end()) return 0; n=std::min(n,it->second.size()); memcpy(out,it->second.data(),n); return n; }
  size_t putBytes(const char* k, const void* in, size_t n) { auto p=(const uint8_t*)in; data[k]={p,p+n}; return n; }
  bool getBool(const char* k, bool def) { return data[k].empty() ? def : data[k][0]; }
  size_t putBool(const char* k, bool v) { uint8_t b=v; return putBytes(k,&b,1); }
  int16_t getShort(const char* k, int16_t def) { int16_t v=def; getBytes(k,&v,sizeof(v)); return v; }
  size_t putShort(const char* k, int16_t v) { return putBytes(k,&v,sizeof(v)); }
};
