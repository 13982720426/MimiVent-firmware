#include "WifiControlService.h"

#if MIMIVENT_ENABLE_WIFI && MIMIVENT_WIFI_READ_CONTROL
#include "ControlApi.h"
#include "OtaManager.h"
#include "WifiBindingStore.h"
#include "WifiControlLease.h"
#include "WifiDeviceIdentity.h"
#include <SPIFFS.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mbedtls/sha256.h>
#include <memory>
#include <new>
#include <cstring>
#include <strings.h>

namespace {
SemaphoreHandle_t commandMutex = nullptr;
portMUX_TYPE rateMux = portMUX_INITIALIZER_UNLOCKED;
class CommandGuard {
 public:
  CommandGuard() : valid(commandMutex &&
      xSemaphoreTake(commandMutex, portMAX_DELAY) == pdTRUE) {}
  ~CommandGuard() { if (valid) xSemaphoreGive(commandMutex); }
  bool valid;
};
struct Rate { char id[33]; uint32_t at; bool used; };
Rate rates[3]{};
bool rateAllowed(const char* id,uint32_t now){
 portENTER_CRITICAL(&rateMux);
 Rate* selected=nullptr;
 for(auto& r:rates)if(r.used&&!strcmp(r.id,id)){selected=&r;break;}
 if(selected&&uint32_t(now-selected->at)<500){portEXIT_CRITICAL(&rateMux);return false;}
 if(!selected){selected=&rates[0];for(auto& r:rates)if(!r.used||uint32_t(now-r.at)>uint32_t(now-selected->at))selected=&r;}
 strcpy(selected->id,id);selected->used=true;selected->at=now;portEXIT_CRITICAL(&rateMux);return true;
}

struct WriteRecord {
 char id[37]; char token[33]; char path[16]; uint16_t code; uint32_t at; bool used; uint8_t boot;
 uint8_t bodyHash[32]; bool hasBodyHash;
};
constexpr size_t kWriteCapacity=32;
constexpr uint32_t kWriteRetentionMs=600000UL;
WriteRecord writes[kWriteCapacity]{};
struct PersistRecord {
 uint8_t id[16]; uint8_t token[16]; char path[16]; uint16_t code; uint8_t used;
 uint8_t bodyHash[32]; uint8_t hasBodyHash;
};
struct LegacyPersistRecord {
 char id[37]; char token[33]; char path[16]; uint16_t code; uint8_t used;
};
struct PersistSlot {
 uint32_t seq; uint32_t checksum; PersistRecord items[kWriteCapacity];
};
template<size_t N> struct LegacyPersistSlot {uint32_t seq;uint32_t checksum;LegacyPersistRecord items[N];};
static_assert(sizeof(PersistSlot)<=sizeof(LegacyPersistSlot<32>),"request digest storage must fit the previous NVS budget");
nvs_handle_t persistHandle=0;
bool persistOpen=false,persistLoaded=false;
uint32_t persistSeq=0;
bool packId(const char* text,uint8_t* out,bool uuid){
 const size_t length=uuid?36:32;
 if(strnlen(text,length+1)!=length)return false;
 size_t digit=0;memset(out,0,16);
 for(size_t i=0;i<length;++i){
  if(uuid&&(i==8||i==13||i==18||i==23)){if(text[i]!='-')return false;continue;}
  const char c=text[i];int n=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
  if(n<0)return false;
  out[digit/2]|=static_cast<uint8_t>(n<<(digit%2?0:4));++digit;
 }
 return digit==32;
}
void unpackId(const uint8_t* packed,char* out,bool uuid){
 const char* hex="0123456789abcdef";size_t position=0;
 for(size_t i=0;i<32;++i){
  if(uuid&&(i==8||i==12||i==16||i==20))out[position++]='-';
  out[position++]=hex[(packed[i/2]>>(i%2?0:4))&15];
 }
 out[position]='\0';
}
uint32_t checksumOf(const PersistSlot& slot){
 const uint8_t* p=reinterpret_cast<const uint8_t*>(&slot);uint32_t hash=2166136261u;
 for(size_t i=0;i<sizeof(slot);++i){hash^=p[i];hash*=16777619u;}
 return hash;
}
template<size_t N> bool readLegacySlot(const char* key,PersistSlot& slot,size_t length){
 std::unique_ptr<LegacyPersistSlot<N>> storage(new(std::nothrow) LegacyPersistSlot<N>{});
 if(!storage||nvs_get_blob(persistHandle,key,storage.get(),&length)!=ESP_OK)return false;
 auto& old=*storage;const uint32_t expected=old.checksum;old.checksum=0;uint32_t hash=2166136261u;
 const auto* bytes=reinterpret_cast<const uint8_t*>(&old);
 for(size_t i=0;i<sizeof(old);++i){hash^=bytes[i];hash*=16777619u;}
 if(hash!=expected)return false;
 slot={};slot.seq=old.seq;
 for(size_t i=0;i<N;++i){
  const auto& p=old.items[i];auto& target=slot.items[i];
  if(!p.used)continue;
  if(!packId(p.id,target.id,true)||!packId(p.token,target.token,false))return false;
  memcpy(target.path,p.path,sizeof(p.path));target.code=p.code;target.used=p.used;
 }
 slot.checksum=checksumOf(slot);return true;
}
bool readSlot(const char* key,PersistSlot& slot){
 const char* path=!strcmp(key,"req-a")?"/wifi-requests-a.bin":"/wifi-requests-b.bin";
 File file=SPIFFS.open(path,FILE_READ);
 if(file){
  const bool read=file.size()==sizeof(slot)&&file.read(reinterpret_cast<uint8_t*>(&slot),sizeof(slot))==sizeof(slot);
  file.close();if(!read)return false;
  const uint32_t expected=slot.checksum;slot.checksum=0;
  const bool valid=expected==checksumOf(slot);slot.checksum=expected;return valid;
 }
 if(!persistOpen)return false;
 size_t length=0;
 if(nvs_get_blob(persistHandle,key,nullptr,&length)!=ESP_OK)return false;
 if(length==sizeof(LegacyPersistSlot<8>))return readLegacySlot<8>(key,slot,length);
 if(length==sizeof(LegacyPersistSlot<32>))return readLegacySlot<32>(key,slot,length);
 if(length!=sizeof(slot)||nvs_get_blob(persistHandle,key,&slot,&length)!=ESP_OK)return false;
 const uint32_t expected=slot.checksum;slot.checksum=0;
 const bool valid=expected==checksumOf(slot);slot.checksum=expected;return valid;
}
// 临时缓冲只在恢复期间存在，避免常驻占用通信所需堆内存。
void loadWrites(){
 if(persistLoaded)return;
 persistOpen=nvs_open("mv-p4req",NVS_READWRITE,&persistHandle)==ESP_OK;
 std::unique_ptr<PersistSlot> aStorage(new(std::nothrow) PersistSlot{}),bStorage(new(std::nothrow) PersistSlot{});
 if(!aStorage||!bStorage){
  // 恢复未完成时禁止控制写入，由启动流程重试，不能覆盖尚未恢复的记录。
  Serial.println("[wifi-write] load skipped: low heap");
  return;
 }
 PersistSlot& a=*aStorage;PersistSlot& b=*bStorage;
 const bool hasA=readSlot("req-a",a),hasB=readSlot("req-b",b);
 const PersistSlot* chosen=nullptr;
 if(hasA&&hasB)chosen=a.seq>=b.seq?&a:&b;else if(hasA)chosen=&a;else if(hasB)chosen=&b;
 persistLoaded=true;
 if(!chosen)return;
 persistSeq=chosen->seq;
 const uint32_t now=millis();
 for(size_t i=0;i<kWriteCapacity;++i){
  const auto& p=chosen->items[i];if(!p.used)continue;
  WriteRecord& w=writes[i];memset(&w,0,sizeof(w));
  unpackId(p.id,w.id,true);unpackId(p.token,w.token,false);
  strncpy(w.path,p.path,sizeof(w.path)-1);w.code=p.code;w.used=true;w.at=now;w.boot=0;
  memcpy(w.bodyHash,p.bodyHash,sizeof(w.bodyHash));w.hasBodyHash=p.hasBodyHash==1;
 }
 Serial.printf("[wifi-write] restored %u request records\n",(unsigned)persistSeq);
}
bool saveWrites(){
 // 恢复失败时不能覆盖未知历史；等待启动流程重试恢复。
 if(!persistLoaded)return false;
 std::unique_ptr<PersistSlot> storage(new(std::nothrow) PersistSlot{});
 if(!storage){
  Serial.println("[wifi-write] persist buffer allocation failed");
  return false;
 }
 PersistSlot& slot=*storage;
 memset(&slot,0,sizeof(slot));
 slot.seq=persistSeq+1;
 for(size_t i=0;i<kWriteCapacity;++i){
  const auto& w=writes[i];if(!w.used)continue;
  auto& p=slot.items[i];
  if(!packId(w.id,p.id,true)||!packId(w.token,p.token,false)){
   Serial.printf("[wifi-write] persist invalid record index=%u idLength=%u tokenLength=%u\n",unsigned(i),unsigned(strnlen(w.id,sizeof(w.id))),unsigned(strnlen(w.token,sizeof(w.token))));
   return false;
  }
  strncpy(p.path,w.path,sizeof(p.path)-1);p.code=w.code;p.used=1;
  memcpy(p.bodyHash,w.bodyHash,sizeof(p.bodyHash));p.hasBodyHash=w.hasBodyHash?1:0;
 }
 slot.checksum=checksumOf(slot);
 // 与统计快照使用不同文件名；只覆盖非当前槽，断电时保留上一份完整记录。
 const char* key=(slot.seq&1)?"req-b":"req-a";
 const char* path=(slot.seq&1)?"/wifi-requests-b.bin":"/wifi-requests-a.bin";
 File file=SPIFFS.open(path,FILE_WRITE);
 if(!file){Serial.println("[wifi-write] request file open failed");return false;}
 const bool written=file.write(reinterpret_cast<const uint8_t*>(&slot),sizeof(slot))==sizeof(slot);
 file.flush();file.close();
 const uint32_t expectedSeq=slot.seq;
 if(!written||!readSlot(key,slot)||slot.seq!=expectedSeq){
  Serial.println("[wifi-write] request file write/verify failed");
  return false;
 }
 persistSeq=expectedSeq;
 // 仅在文件写入并读回验证后迁移，保留设备身份、绑定及 MQTT 配置。
 if(persistOpen){
  nvs_erase_key(persistHandle,"req-a");nvs_erase_key(persistHandle,"req-b");nvs_commit(persistHandle);
  nvs_close(persistHandle);persistHandle=0;persistOpen=false;
 }
 return true;
}

String formValue(const String& body,const char* key){
 const String prefix=String(key)+"=";int start=0;
 while(start<(int)body.length()){
  const int end=body.indexOf('&',start);
  const String item=end<0?body.substring(start):body.substring(start,end);
  if(item.startsWith(prefix))return item.substring(prefix.length());
  if(end<0)break;start=end+1;
 }
 return String();
}

bool uuidValid(const char* s){
 if(strlen(s)!=36)return false;
 for(size_t i=0;i<36;++i){bool dash=i==8||i==13||i==18||i==23;
  if(dash?(s[i]!='-'):(!isxdigit((unsigned char)s[i])))return false;}
 return true;
}

int findWrite(const char* id,const char* token){
 for(size_t i=0;i<kWriteCapacity;++i)if(writes[i].used&&!strcasecmp(writes[i].id,id)&&!strcasecmp(writes[i].token,token))return i;
 return -1;
}
const char* writeConflict(const WriteRecord& record,const char* path,const uint8_t* digest){
 if(strcmp(record.path,path))return "REQUEST_ID_CONFLICT";
 // 旧记录没有摘要，只能查询历史结果，不能把任意参数复用误报成成功。
 if(!record.hasBodyHash)return "REQUEST_BODY_UNKNOWN";
 return memcmp(record.bodyHash,digest,32)?"REQUEST_ID_CONFLICT":nullptr;
}
int beginWrite(const char* id,const char* token,const char* path,const uint8_t* digest,uint32_t now){
 int slot=-1;
 for(size_t i=0;i<kWriteCapacity;++i)if(!writes[i].used){slot=i;break;}
 if(slot<0){
  // 恢复记录仍可能对应 App 保存的待确认请求；重启后至少保留完整去重窗口，
  // 不因 boot=0 就立即覆盖，避免已执行命令被相同 requestId 再次执行。
  for(size_t i=0;i<kWriteCapacity;++i)
   if(!writes[i].boot&&uint32_t(now-writes[i].at)>kWriteRetentionMs){slot=i;break;}
 }
 if(slot<0){
  for(size_t i=0;i<kWriteCapacity;++i)if(uint32_t(now-writes[i].at)>kWriteRetentionMs){slot=i;break;}
 }
 if(slot<0){
  // 表满时优先回收最旧的终态记录。未知/处理中记录仍保留完整保护窗口，
  // 避免用户重试时把可能已执行的请求再次执行；终态结果仍由 Gateway/App
  // 缓存，设备端只在容量压力下让出槽位，防止连续控制后长期返回 429。
  uint32_t oldestAge=0;
  for(size_t i=0;i<kWriteCapacity;++i){
   if(writes[i].code==0)continue;
   const uint32_t age=uint32_t(now-writes[i].at);
   if(slot<0||age>oldestAge){slot=static_cast<int>(i);oldestAge=age;}
  }
  if(slot>=0){
   Serial.printf("[wifi-write] recycle completed record index=%d status=%u age_ms=%lu\n",
                 slot,unsigned(writes[slot].code),static_cast<unsigned long>(oldestAge));
  }
 }
 if(slot<0)return -2; // 仅当保护窗口内的记录全都仍待确认时拒绝新控制。
 const WriteRecord previous=writes[slot];
 WriteRecord& w=writes[slot];memset(&w,0,sizeof(w));
 strncpy(w.id,id,sizeof(w.id)-1);strncpy(w.token,token,sizeof(w.token)-1);
 strncpy(w.path,path,sizeof(w.path)-1);w.code=0;w.at=now;w.used=true;w.boot=1;
 memcpy(w.bodyHash,digest,sizeof(w.bodyHash));w.hasBodyHash=true;
 if(!saveWrites()){writes[slot]=previous;return -1;}
 return slot;
}
void completeWrite(int slot,uint16_t code,const char* body){
 if(slot<0||slot>=kWriteCapacity)return;
 writes[slot].code=code;writes[slot].boot=1;
 saveWrites();
}
}

void wifiControlServiceBegin() {
  if (!commandMutex) commandMutex = xSemaphoreCreateMutex();
  if (!commandMutex) {
    Serial.println("[wifi-control] command mutex create failed");
    return;
  }
  loadWrites();
  Serial.printf("[wifi-control] command channel ready mutex=%p persistenceLoaded=%d\n",
                static_cast<void*>(commandMutex), persistLoaded ? 1 : 0);
}

void wifiControlServiceUpdate() {
  if (otaManagerIsActive()) return;
  wifiBindingUpdate();
  static uint32_t lastMutexAttempt = 0;
  const uint32_t now = millis();
  if (!commandMutex && uint32_t(now - lastMutexAttempt) >= 5000UL) {
    lastMutexAttempt = now;
    commandMutex = xSemaphoreCreateMutex();
    if (commandMutex) Serial.println("[wifi-control] command mutex recovered");
  }
  if (persistLoaded) return;
  static uint32_t lastRestoreAttempt = 0;
  if (uint32_t(now - lastRestoreAttempt) < 5000UL) return;
  lastRestoreAttempt = now;
  loadWrites();
}

bool wifiControlServiceCommand(const char* token,const char* requestId,const char* path,const char* body,uint32_t issuedAtMs,uint16_t* statusCode,String* responseBody){
 if(!statusCode||!responseBody||!commandMutex){
  Serial.printf("[mqtt-cmd] precheck failed status=%d body=%d mutex=%p\n",
                statusCode?1:0,responseBody?1:0,(void*)commandMutex);
  return false;
 }
 CommandGuard commandGuard;if(!commandGuard.valid)return false;
 const uint32_t now=millis();
 const auto auth=wifiBindingAuthorize(token,now);
 if(!auth.valid||auth.pending){*statusCode=401;*responseBody="{\"error\":\"BINDING_INVALID\"}";return true;}
 if(!uuidValid(requestId)){*statusCode=400;*responseBody="{\"error\":\"INVALID_REQUEST_ID\"}";return true;}
 const bool controlPath=!strcmp(path,"/api/auto")||!strcmp(path,"/api/timed")||!strcmp(path,"/api/test-fan")||!strcmp(path,"/api/lockoff");
 const bool settingsPath=!strcmp(path,"/api/settings");
 const bool leaseReleasePath=!strcmp(path,"/api/lease");
 if((!controlPath&&!settingsPath&&!leaseReleasePath)||!body||strlen(body)>256){*statusCode=400;*responseBody="{\"error\":\"INVALID_REQUEST\"}";return true;}
 uint8_t digest[32];
 if(mbedtls_sha256(reinterpret_cast<const uint8_t*>(body),strlen(body),digest,0)!=0){*statusCode=503;*responseBody="{\"error\":\"DEDUP_UNAVAILABLE\"}";return true;}
 const int existing=findWrite(requestId,auth.tokenId);
 if(existing>=0){
  const char* conflict=writeConflict(writes[existing],path,digest);
  if(conflict){*statusCode=409;*responseBody=String("{\"error\":\"")+conflict+"\"}";return true;}
  if(writes[existing].code==0){*statusCode=409;*responseBody=writes[existing].boot?"{\"error\":\"REQUEST_IN_PROGRESS\"}":"{\"error\":\"REQUEST_UNKNOWN\"}";return true;}
  if(writes[existing].code<400){*statusCode=200;*responseBody="{\"ok\":true,\"duplicate\":true}";return true;}
  *statusCode=writes[existing].code;*responseBody="{\"error\":\"DUPLICATE\"}";return true;
 }
 // 两阶段握手（prepare→execute）要走两次 App↔gateway↔MQTT↔设备 往返，
 // 实测单次 400-800 ms。原来 1 秒窗口装不下这个往返，导致 COMMAND_EXPIRED
 // 频繁出现，表现为"点了没反应/闪烁/重试"。防重放仍由 requestId 去重
 // （10 分钟窗口）与 HMAC freshnessProof 保证，放宽时限不影响安全性。
 if(uint32_t(now-issuedAtMs)>3000){*statusCode=409;*responseBody="{\"error\":\"COMMAND_EXPIRED\"}";return true;}
 if(otaManagerIsActive()){*statusCode=423;*responseBody="{\"error\":\"OTA_BUSY\"}";return true;}
 if(!leaseReleasePath&&!wifiControlLeaseAcquire(auth.tokenId,now)){*statusCode=423;*responseBody="{\"error\":\"WIFI_IN_USE\"}";return true;}
 if(!rateAllowed(auth.tokenId,now)){*statusCode=429;*responseBody="{\"error\":\"RATE_LIMITED\"}";return true;}
 const int pendingSlot=beginWrite(requestId,auth.tokenId,path,digest,now);
 if(pendingSlot==-2){*statusCode=429;*responseBody="{\"error\":\"DEDUP_WINDOW_FULL\"}";return true;}
 if(pendingSlot<0){*statusCode=503;*responseBody="{\"error\":\"DEDUP_UNAVAILABLE\"}";return true;}
 if(leaseReleasePath){
  if(strcmp(body,"action=release")){completeWrite(pendingSlot,400,"{\"error\":\"INVALID_LEASE_ACTION\"}");*statusCode=400;*responseBody="{\"error\":\"INVALID_LEASE_ACTION\"}";return true;}
  wifiControlLeaseRelease(auth.tokenId);
  completeWrite(pendingSlot,200,"{\"released\":true}");
  *statusCode=200;*responseBody="{\"released\":true}";return true;
 }
 ControlApiRequest request;request.method="POST";request.path=path;request.body=body;
 const String bodyText(body);
 const String profile=formValue(bodyText,"profile");
 if(profile.length()&&!settingsStoreParseProfile(profile,&request.profile)){
  completeWrite(pendingSlot,400,"{\"error\":\"INVALID_PROFILE\"}");*statusCode=400;*responseBody="{\"error\":\"INVALID_PROFILE\"}";return true;
 }
 request.hasDeadline=true;request.issuedAtMs=issuedAtMs;request.maxAgeMs=3000;
 request.mode=formValue(bodyText,"mode");request.stage=formValue(bodyText,"stage");
 request.replace=formValue(bodyText,"replace")=="1";request.switchProfile=formValue(bodyText,"switchProfile")=="1";
 if(settingsPath){
  request.hasSettings=true;
  const bool valid=controlApiParseTimeValue(formValue(bodyText,"startupCooldownMs"),false,&request.settings.startupCooldownMs)&&
   controlApiParseTimeValue(formValue(bodyText,"leaveConfirmMs"),true,&request.settings.leaveConfirmMs)&&
   controlApiParseTimeValue(formValue(bodyText,"autoExhaustMs"),false,&request.settings.autoExhaustMs)&&
   controlApiParseTimeValue(formValue(bodyText,"manualExhaustMs"),false,&request.settings.manualExhaustMs)&&
   controlApiParseTimeValue(formValue(bodyText,"pauseMs"),false,&request.settings.pauseMs);
  if(!valid){completeWrite(pendingSlot,400,"{\"error\":\"INVALID_SETTINGS\"}");*statusCode=400;*responseBody="{\"error\":\"INVALID_SETTINGS\"}";return true;}
 }
 const auto result=controlApiDispatch(request);
 completeWrite(pendingSlot,result.statusCode,result.body.c_str());
 *statusCode=result.statusCode;*responseBody=result.body;
 return true;
}
#endif
