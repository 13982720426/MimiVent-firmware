#include "WifiBindingStore.h"
#if MIMIVENT_ENABLE_WIFI && MIMIVENT_WIFI_READ_CONTROL
#include "WifiBindingCore.h"
#include "WifiDeviceIdentity.h"
#include "WifiHttpsServer.h"
#include "WifiStorageBudget.h"
#include "OtaCheckpointStore.h"
#include <WiFi.h>
#include <esp_random.h>
#include <mbedtls/sha256.h>
#include <nvs.h>
#include <freertos/queue.h>
#include <atomic>
namespace {
using namespace WifiBindings;
static_assert(sizeof(OtaCheckpoint)==704,"reassess NVS reserve when checkpoint layout changes");
Core core;
nvs_handle_t storage;
bool storageOpen=false;std::atomic<bool> ready{false};
const char* storeState="UNINITIALIZED";
portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
Record authActive{}; Pending authPending{};
QueueHandle_t verifiedQueue=nullptr;
struct Verified { char id[33]; };
void publish() {
 portENTER_CRITICAL(&mux); authActive=core.active;authPending=core.pending;portEXIT_CRITICAL(&mux);
}
String encode(cJSON* value) {char* raw=cJSON_PrintUnformatted(value);String s=raw?raw:"{\"error\":\"MEMORY\"}";cJSON_free(raw);cJSON_Delete(value);return s;}
String error(const char* text) {auto* v=cJSON_CreateObject();cJSON_AddStringToObject(v,"error",text);return encode(v);}
const char* field(cJSON* r,const char* name) {auto* v=cJSON_GetObjectItemCaseSensitive(r,name);return cJSON_IsString(v)?v->valuestring:"";}
bool label(const char* s,size_t max) {size_t n=strlen(s);if(!n||n>max)return false;for(size_t i=0;i<n;++i)if(!isalnum((unsigned char)s[i])&&s[i]!='-'&&s[i]!='_')return false;return true;}
bool uuidValid(const char* s){
 if(strlen(s)!=36)return false;
 for(size_t i=0;i<36;++i){bool dash=i==8||i==13||i==18||i==23;if(dash?s[i]!='-':!isxdigit((unsigned char)s[i]))return false;}
 return true;
}
bool writeSlot(const char* slot,const Record& r) {
 if(nvs_set_blob(storage,slot,&r,sizeof(r))!=ESP_OK||nvs_commit(storage)!=ESP_OK)return false;
 Record check{};size_t n=sizeof(check);
 return nvs_get_blob(storage,slot,&check,&n)==ESP_OK&&n==sizeof(check)&&valid(check)&&!memcmp(&check,&r,sizeof(r));
}
bool empty(const Record& r){for(const auto& e:r.entries)if(e.used)return false;return true;}
bool removeEmptySlots(){
 for(const char* key:{"slot-a","slot-b"}){const auto rc=nvs_erase_key(storage,key);if(rc!=ESP_OK&&rc!=ESP_ERR_NVS_NOT_FOUND)return false;}
 return nvs_commit(storage)==ESP_OK;
}
bool save(const Record& r,void*) {
 // 成功响应前镜像到两个槽，避免最新槽以后损坏时恢复已撤销 token。
 const char* first=(r.sequence&1)?"slot-b":"slot-a";
 const char* second=(r.sequence&1)?"slot-a":"slot-b";
 if(writeSlot(first,r)&&writeSlot(second,r)){
  // 两槽均已确认空表后才删除；断电只会留下空表，不会复活已撤销 token。
  if(empty(r)&&!removeEmptySlots()){storeState="STORE_UNCONFIRMED";ready=false;return false;}
  nvs_stats_t stats{};if(nvs_get_stats(nullptr,&stats)==ESP_OK)
   Serial.printf("[wifi-bind] sequence=%lu nvs-used=%u nvs-free=%u\n",(unsigned long)r.sequence,stats.used_entries,stats.free_entries);
  return true;
 }
 storeState="STORE_UNCONFIRMED";ready=false;return false;
}
bool load(const char* key,Record& r,bool& exists) {
 size_t n=0;const auto rc=nvs_get_blob(storage,key,nullptr,&n);exists=rc!=ESP_ERR_NVS_NOT_FOUND;
 if(!exists)return true;
 return rc==ESP_OK&&n==sizeof(r)&&nvs_get_blob(storage,key,&r,&n)==ESP_OK;
}
void randomHex(char* output,size_t bytes) {uint8_t raw[16];esp_fill_random(raw,bytes);for(size_t i=0;i<bytes;++i)snprintf(output+i*2,3,"%02x",raw[i]);}
void randomToken(char token[44]) {
 constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
 uint8_t raw[32];esp_fill_random(raw,sizeof(raw));uint32_t bits=0;int count=0;size_t out=0;
 for(uint8_t b:raw){bits=(bits<<8)|b;count+=8;while(count>=6){count-=6;token[out++]=alphabet[(bits>>count)&63];}}
 if(count)token[out++]=alphabet[(bits<<(6-count))&63];token[out]=0;
}
}
void wifiBindingBegin() {
 // 只记录命名空间占用，不读取/输出凭据值，用于 20 KiB NVS 容量验收。
 nvs_iterator_t iterator=nullptr;char seen[24][16]{};size_t seenCount=0;
 esp_err_t iteratorResult=nvs_entry_find("nvs",nullptr,NVS_TYPE_ANY,&iterator);
 while(iteratorResult==ESP_OK){
  nvs_entry_info_t info{};nvs_entry_info(iterator,&info);bool known=false;
  if(!strcmp(info.namespace_name,"mv-wifi-v1")){
   nvs_handle_t handle;if(nvs_open(info.namespace_name,NVS_READONLY,&handle)==ESP_OK){size_t length=0;
    if(nvs_get_blob(handle,info.key,nullptr,&length)==ESP_OK)
      Serial.printf("[nvs-budget] legacy-namespace-key=%s blobBytes=%u\n",info.key,length);
    nvs_close(handle);}
  }
  for(size_t i=0;i<seenCount;++i)if(!strcmp(seen[i],info.namespace_name))known=true;
  if(!known&&seenCount<24){
   strncpy(seen[seenCount++],info.namespace_name,15);nvs_handle_t handle;
   if(nvs_open(info.namespace_name,NVS_READONLY,&handle)==ESP_OK){size_t used=0;nvs_get_used_entry_count(handle,&used);nvs_close(handle);
    Serial.printf("[nvs-budget] namespace=%s entries=%u\n",info.namespace_name,used);}
  }
  iteratorResult=nvs_entry_next(&iterator);
 }
 nvs_release_iterator(iterator);
 verifiedQueue=xQueueCreate(2,sizeof(Verified));
 if(!verifiedQueue){storeState="MEMORY_UNAVAILABLE";return;}
 if(nvs_open("mv-bindings",NVS_READWRITE,&storage)!=ESP_OK){storeState="STORAGE_UNAVAILABLE";return;}
 storageOpen=true;Record a{},b{};bool ae=false,be=false;
 if(!load("slot-a",a,ae)||!load("slot-b",b,be)){storeState="RECORD_UNREADABLE";return;}
 if((ae&&a.version!=1)||(be&&b.version!=1)){storeState="MIGRATION_REQUIRED";return;}
 const bool av=ae&&valid(a),bv=be&&valid(b);
 if((ae||be)&&!av&&!bv){storeState="BINDINGS_CORRUPT";return;}
 if(av&&bv&&a.sequence==b.sequence&&memcmp(&a,&b,sizeof(a))){storeState="BINDINGS_CORRUPT";return;}
 if(av||bv)core.active=(!bv||(av&&a.sequence>=b.sequence))?a:b;
 if(av||bv){
  if((!av||memcmp(&a,&core.active,sizeof(a)))&&!writeSlot("slot-a",core.active)){storeState="STORE_UNCONFIRMED";return;}
  if((!bv||memcmp(&b,&core.active,sizeof(b)))&&!writeSlot("slot-b",core.active)){storeState="STORE_UNCONFIRMED";return;}
  if(empty(core.active)&&!removeEmptySlots()){storeState="STORE_UNCONFIRMED";return;}
 }
 ready=true;storeState="READY";publish();
 nvs_stats_t stats{};if(nvs_get_stats(nullptr,&stats)==ESP_OK)
  Serial.printf("[wifi-bind] ready sequence=%lu nvs-used=%u nvs-free=%u\n",(unsigned long)core.active.sequence,stats.used_entries,stats.free_entries);
}
void wifiBindingUpdate(){
 if(!ready)return;core.tick(millis());Verified event{};
 while(xQueueReceive(verifiedQueue,&event,0)==pdTRUE)
  if(core.pending.exists&&!strcmp(core.pending.entry.id,event.id))core.pending.verified=true;
 publish();
}
bool wifiBindingReady(){return ready;}
const char* wifiBindingState(){return storeState;}
WifiAuthorization wifiBindingAuthorize(const char* token,uint32_t now){
 WifiAuthorization result{};if(!ready||strlen(token)!=43)return result;
 for(size_t i=0;i<43;++i)if(!isalnum((unsigned char)token[i])&&token[i]!='-'&&token[i]!='_')return result;
 uint8_t hash[32];mbedtls_sha256(reinterpret_cast<const uint8_t*>(token),43,hash,0);
 portENTER_CRITICAL(&mux);
 for(const auto& e:authActive.entries)if(e.used&&equalHash(hash,e.hash)){result.valid=true;strcpy(result.tokenId,e.id);}
 if(!result.valid&&authPending.exists&&uint32_t(now-authPending.started)<kTtlMs&&equalHash(hash,authPending.entry.hash)){
  result.valid=true;result.pending=true;strcpy(result.tokenId,authPending.entry.id);
 }
 portEXIT_CRITICAL(&mux);return result;
}
bool wifiBindingOwnerAllowed(const char* owner){
 if(!ready||!owner)return false;
 bool any=false;bool allowed=false;
 portENTER_CRITICAL(&mux);
 for(const auto& e:authActive.entries)if(e.used){any=true;if(!strcmp(e.owner,owner))allowed=true;}
 portEXIT_CRITICAL(&mux);
 return !any||allowed;
}
bool wifiBindingRevokeAllowed(const char* owner,const char* tokenId){
 if(!ready||!owner||!tokenId)return false;
 bool found=false;bool allowed=false;
 portENTER_CRITICAL(&mux);
 for(const auto& e:authActive.entries)if(e.used&&!strcmp(e.id,tokenId)){found=true;allowed=!strcmp(e.owner,owner);}
 portEXIT_CRITICAL(&mux);
 return !found||allowed;
}
bool wifiBindingMarkVerified(const char* tokenId){Verified event{};strncpy(event.id,tokenId,32);return verifiedQueue&&xQueueSend(verifiedQueue,&event,0)==pdTRUE;}
String wifiBindingRequest(cJSON* root,const char* owner){
 if(!ready)return error(storeState);
 if(!owner||strlen(owner)!=14||owner[1]!='-')return error("PAIRING_REQUIRED");
 wifiBindingUpdate();const char* op=field(root,"op");const char* id=field(root,"tokenId");
 auto* out=cJSON_CreateObject();const char* failure=nullptr;const auto now=millis();
 if(!strcmp(op,"wifi.bind.open")){
  if(!wifiHttpsServerReady())failure="WIFI_UNAVAILABLE";
  else failure=core.open(owner,now);
  cJSON_AddNumberToObject(out,"expiresInSec",120);
 }else if(!strcmp(op,"wifi.bind.begin")){
  nvs_stats_t stats{};bool hasTable=false;for(const auto& entry:core.active.entries)hasTable|=entry.used!=0;
  if(!wifiHttpsServerReady())failure="WIFI_UNAVAILABLE";
  else if(nvs_get_stats(nullptr,&stats)!=ESP_OK)failure="STORAGE_UNAVAILABLE";
  else if(!core.pending.exists&&!WifiStorageBudget::allowsBinding(stats.free_entries,hasTable))failure="WIFI_STORAGE_LOW";
  else if(!label(field(root,"clientId"),36)||!uuidValid(field(root,"requestId")))failure="INVALID_REQUEST";
  else{
   Entry e{};e.used=1;strcpy(e.client,field(root,"clientId"));strcpy(e.owner,owner);randomHex(e.id,16);
   char token[44];randomToken(token);mbedtls_sha256(reinterpret_cast<const uint8_t*>(token),43,e.hash,0);
   failure=core.begin(e,field(root,"requestId"),token,cJSON_IsTrue(cJSON_GetObjectItem(root,"replace")),now);
   memset(token,0,sizeof(token));
   if(!failure){
    cJSON_AddStringToObject(out,"scheme","https");cJSON_AddStringToObject(out,"host",WiFi.localIP().toString().c_str());cJSON_AddNumberToObject(out,"port",8443);
    cJSON_AddStringToObject(out,"deviceId",wifiDeviceId());cJSON_AddStringToObject(out,"certPinSha256",wifiDevicePin());
    cJSON_AddStringToObject(out,"tokenId",core.pending.entry.id);cJSON_AddStringToObject(out,"token",core.pending.token);
    cJSON_AddStringToObject(out,"bindingState","pending");cJSON_AddNumberToObject(out,"expiresInSec",(kTtlMs-uint32_t(now-core.pending.started))/1000);
   }
  }
 }else if(!strcmp(op,"wifi.bind.status")){
  const char* state=core.status(owner,id,now);
  if(!strcmp(state,"forbidden"))failure="PERMISSION_DENIED";
  else {cJSON_AddStringToObject(out,"bindingState",state);cJSON_AddStringToObject(out,"tokenId",id);}
 }else if(!strcmp(op,"wifi.bind.commit")){
  failure=core.commit(owner,id,now,save,nullptr);cJSON_AddStringToObject(out,"bindingState","active");
 }else if(!strcmp(op,"wifi.bind.cancel"))failure=core.cancel(owner,id);
 else if(!strcmp(op,"wifi.bind.reset")){
  if(strcmp(field(root,"confirm"),"RESET_ALL_BINDINGS"))failure="CONFIRMATION_REQUIRED";
  else {
   failure=core.reset(save,nullptr);
   if(!failure){wifiHttpsResetLease();cJSON_AddStringToObject(out,"bindingState","reset");}
  }
 }
 else if(!strcmp(op,"wifi.revoke")){
  if(!wifiBindingRevokeAllowed(owner,id))failure="PERMISSION_DENIED";
  else {failure=core.revoke(id,save,nullptr);cJSON_AddStringToObject(out,"bindingState","revoked");}
 }
 else if(!strcmp(op,"wifi.clients")){
  auto* list=cJSON_AddArrayToObject(out,"clients");for(const auto& e:core.active.entries)if(e.used&&!strcmp(e.owner,owner)){auto* v=cJSON_CreateObject();cJSON_AddStringToObject(v,"tokenId",e.id);cJSON_AddStringToObject(v,"clientId",e.client);cJSON_AddItemToArray(list,v);}
 }else failure="UNSUPPORTED_OPERATION";
 if(failure&&!strcmp(failure,"WIFI_STORAGE_LOW")&&!core.pending.exists)core.window=false;
 publish();if(failure){cJSON_Delete(out);return error(failure);}return encode(out);
}
#endif
