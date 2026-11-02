#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "WifiIdentityRecord.h"

// owner loop 独占状态；HTTP 任务仅读取其独立副本。
namespace WifiBindings {
constexpr uint32_t kTtlMs = 120000;
struct Entry { uint8_t used; char client[37], owner[18], id[33]; uint8_t hash[32]; };
struct Record {
  uint32_t magic; uint16_t version, bytes; uint32_t sequence;
  Entry entries[2]; uint16_t reserved; uint32_t checksum;
};
static_assert(sizeof(Record) == 260 && offsetof(Record, checksum) == 256, "binding layout changed");
struct Pending { Entry entry; char token[44], request[37]; uint32_t started; bool exists, verified; };
inline bool equalHash(const uint8_t* a, const uint8_t* b) {
  uint8_t difference = 0; for (size_t i=0;i<32;++i) difference |= a[i]^b[i]; return difference==0;
}
inline uint32_t crc(const uint8_t* p, size_t length) {
  uint32_t v=UINT32_MAX;
  for(size_t i=0;i<length;++i) { v^=p[i]; for(int b=0;b<8;++b) v=(v>>1)^(0xedb88320U & (0U-(v&1))); }
  return ~v;
}
inline bool valid(const Record& r) {
  if(r.magic!=0x3142564d || r.version!=1 || r.bytes!=sizeof(r) || !r.sequence ||
     r.reserved || r.checksum!=crc(reinterpret_cast<const uint8_t*>(&r),offsetof(Record,checksum))) return false;
  for(const auto& e:r.entries) if(e.used && (e.used!=1 || !e.client[0] || !e.owner[0] ||
      !e.id[0] || !memchr(e.client,0,sizeof(e.client)) || !memchr(e.owner,0,sizeof(e.owner)) ||
      !memchr(e.id,0,sizeof(e.id)) || strlen(e.id)!=32)) return false;
  return true;
}
inline void seal(Record& r) {
  r.magic=0x3142564d; r.version=1; r.bytes=sizeof(r); r.reserved=0;
  r.checksum=crc(reinterpret_cast<const uint8_t*>(&r),offsetof(Record,checksum));
}
class Core {
 public:
  Record active{}; Pending pending{};
  char windowOwner[18]{}; uint32_t windowAt=0; bool window=false;
  using Save = bool (*)(const Record&,void*);
  void tick(uint32_t now) {
    if(window && uint32_t(now-windowAt)>=kTtlMs) window=false;
    if(pending.exists && uint32_t(now-pending.started)>=kTtlMs) pending={};
  }
  const char* open(const char* owner,uint32_t now) {
    tick(now);
    if(window && strcmp(windowOwner,owner)) return "BIND_BUSY";
    if(pending.exists && strcmp(pending.entry.owner,owner)) return "BIND_BUSY";
    strncpy(windowOwner,owner,sizeof(windowOwner)-1); windowAt=now; window=true; return nullptr;
  }
  const char* begin(const Entry& e,const char* request,const char* token,bool replace,uint32_t now) {
    tick(now);
    if(!window || strcmp(windowOwner,e.owner)) return "BIND_WINDOW_CLOSED";
    if(pending.exists) {
      return !strcmp(pending.entry.owner,e.owner) && !strcmp(pending.entry.client,e.client) &&
          !strcmp(pending.request,request) ? nullptr : "BIND_BUSY";
    }
    int used=0; bool replacement=false;
    for(const auto& old:active.entries) if(old.used) {
      ++used;
      if(!strcmp(old.client,e.client)) {
        if(strcmp(old.owner,e.owner)) return "CLIENT_OWNED";
        if(!replace) return "REPLACE_REQUIRED";
        replacement=true;
      }
    }
    if(used==2 && !replacement) return "BINDINGS_FULL";
    pending={}; pending.entry=e; pending.entry.used=1;
    strncpy(pending.request,request,sizeof(pending.request)-1);
    strncpy(pending.token,token,sizeof(pending.token)-1);
    pending.started=now; pending.exists=true; return nullptr;
  }
  const char* status(const char* owner,const char* id,uint32_t now) {
    tick(now);
    for(const auto& e:active.entries) if(e.used && !strcmp(e.id,id))
      return !strcmp(e.owner,owner) ? "active" : "forbidden";
    if(pending.exists && !strcmp(pending.entry.id,id))
      return !strcmp(pending.entry.owner,owner) ? "pending" : "forbidden";
    return "expired";
  }
  const char* commit(const char* owner,const char* id,uint32_t now,Save save,void* context) {
    tick(now);
    const char* current=status(owner,id,now);
    if(!strcmp(current,"active")) return nullptr;
    if(strcmp(current,"pending")) return "BINDING_INVALID";
    if(!window || strcmp(windowOwner,owner)) return "BIND_WINDOW_CLOSED";
    if(!pending.verified) return "VERIFICATION_REQUIRED";
    if(active.sequence==UINT32_MAX) return "SEQUENCE_EXHAUSTED";
    Record next=active; int slot=-1;
    for(int i=0;i<2;++i) if(next.entries[i].used && !strcmp(next.entries[i].client,pending.entry.client)) slot=i;
    if(slot<0) for(int i=0;i<2;++i) if(!next.entries[i].used) {slot=i;break;}
    if(slot<0) return "BINDINGS_FULL";
    next.entries[slot]=pending.entry; ++next.sequence; seal(next);
    if(!save(next,context)) return "BINDINGS_STORE_FAILED";
    active=next; pending={}; window=false; return nullptr;
  }
  const char* cancel(const char* owner,const char* id) {
    if(pending.exists && !strcmp(pending.entry.id,id)) {
      if(strcmp(pending.entry.owner,owner)) return "PERMISSION_DENIED";
      pending={};window=false;
    }
    if(!pending.exists&&window&&!strcmp(windowOwner,owner))window=false;
    return nullptr;
  }
  const char* reset(Save save,void* context) {
    if(active.sequence==UINT32_MAX) return "SEQUENCE_EXHAUSTED";
    Record next=active;
    for(auto& e:next.entries) e={};
    ++next.sequence; seal(next);
    if(!save(next,context)) return "BINDINGS_STORE_FAILED";
    active=next; pending={}; window=false; return nullptr;
  }
  const char* revoke(const char* id,Save save,void* context) {
    Record next=active; bool changed=false;
    for(auto& e:next.entries) if(e.used && !strcmp(e.id,id)) {e={};changed=true;}
    if(!changed) return nullptr;
    if(next.sequence==UINT32_MAX) return "SEQUENCE_EXHAUSTED";
    ++next.sequence; seal(next); if(!save(next,context)) return "BINDINGS_STORE_FAILED";
    active=next; return nullptr;
  }
};
}
