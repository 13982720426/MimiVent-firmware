#pragma once
#include <map>
#include <vector>
#include "Arduino.h"
#define FILE_READ "r"
#define FILE_WRITE "w"
inline std::map<std::string,std::vector<uint8_t>> files;
inline bool spiffsWriteFailure=false;
class File {
 std::vector<uint8_t>* bytes=nullptr; size_t pos=0;
 public:
 File() = default;
 File(std::vector<uint8_t>* p):bytes(p) {}
 explicit operator bool() const { return bytes; }
 size_t size() { return bytes ? bytes->size() : 0; }
 size_t read(uint8_t* out,size_t n) { if(!bytes) return 0; n=std::min(n,bytes->size()-pos); memcpy(out,bytes->data()+pos,n); pos+=n; return n; }
 size_t write(const uint8_t* in,size_t n) { if(!bytes) return 0; bytes->insert(bytes->end(),in,in+n); return n; }
 void flush() {} void close() {}
};
struct SpiffsMock {
 bool begin(bool,const char*,int,const char*) { return true; }
 bool format() { files.clear(); return true; }
 void end() {}
 File open(const char* p,const char* mode) { if (*mode=='w') {if(spiffsWriteFailure)return File();files[p].clear();} auto it=files.find(p); return it==files.end() ? File() : File(&it->second); }
};
inline SpiffsMock SPIFFS;
