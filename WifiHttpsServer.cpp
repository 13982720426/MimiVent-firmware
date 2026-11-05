#include "WifiHttpsServer.h"
#if MIMIVENT_ENABLE_WIFI && MIMIVENT_WIFI_READ_CONTROL
#include "WifiDeviceIdentity.h"
#include "WifiTlsIdleTracker.h"
#include "ControlApi.h"
#include "OtaManager.h"
#include <WiFi.h>
#include <esp_https_server.h>
#include <esp_heap_caps.h>
#include <lwip/sockets.h>
#include <nvs.h>
namespace {
httpd_handle_t server=nullptr;
char snapshot[1536]{};size_t snapshotLength=0;
uint32_t sampledAt=0,sampleSeq=0,lastStart=0,lastLog=0;
bool otaSuspended=false;
portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
WifiTlsIdleTracker idle;WifiTlsIdleTracker::Work idleWork{};
constexpr uint32_t kWifiLeaseIdleMs=20000;
char leaseToken[33]{};
uint32_t leaseTouchedAt=0;
bool leaseAllowed(const char* token,uint32_t now){
 if(!token||!*token)return false;
 if(!leaseToken[0]||uint32_t(now-leaseTouchedAt)>kWifiLeaseIdleMs||!strcmp(leaseToken,token)){
  strncpy(leaseToken,token,sizeof(leaseToken)-1);leaseTouchedAt=now;return true;
 }
 return false;
}
void leaseRelease(const char* token){if(token&&!strcmp(leaseToken,token)){leaseToken[0]='\0';leaseTouchedAt=0;}}

struct Rate {char id[33];uint32_t at;bool used;};Rate rates[3]{};
esp_err_t fail(httpd_req_t* r,const char* status,const char* reason){
 httpd_resp_set_status(r,status);httpd_resp_set_type(r,"application/json");httpd_resp_set_hdr(r,"Connection","close");
 char body[96];snprintf(body,sizeof(body),"{\"error\":\"%s\"}",reason);return httpd_resp_sendstr(r,body);
}
bool rateAllowed(const char* id,uint32_t now){
 Rate* selected=nullptr;
 for(auto& r:rates)if(r.used&&!strcmp(r.id,id)){selected=&r;break;}
 if(selected&&uint32_t(now-selected->at)<500)return false;
 if(!selected){selected=&rates[0];for(auto& r:rates)if(!r.used||uint32_t(now-r.at)>uint32_t(now-selected->at))selected=&r;}
 strcpy(selected->id,id);selected->used=true;selected->at=now;return true;
}
struct WriteRecord {
 char id[37]; char token[33]; char path[16]; uint16_t code; char body[384]; uint32_t at; bool used; uint8_t boot;
};
WriteRecord writes[8]{};
struct PersistRecord {
 char id[37]; char token[33]; char path[16]; uint16_t code; uint8_t used;
};
struct PersistSlot {
 uint32_t seq; uint32_t checksum; PersistRecord items[8];
};
nvs_handle_t persistHandle=0;
bool persistOpen=false,persistLoaded=false;
uint32_t persistSeq=0;
uint32_t checksumOf(const PersistSlot& slot){
 const uint8_t* p=reinterpret_cast<const uint8_t*>(&slot);uint32_t hash=2166136261u;
 for(size_t i=0;i<sizeof(slot);++i){hash^=p[i];hash*=16777619u;}
 return hash;
}
bool readSlot(const char* key,PersistSlot& slot){
 size_t length=sizeof(slot);
 if(nvs_get_blob(persistHandle,key,&slot,&length)!=ESP_OK||length!=sizeof(slot))return false;
 PersistSlot check=slot;check.checksum=0;return slot.checksum==checksumOf(check);
}
void loadWrites(){
 if(persistLoaded)return;persistLoaded=true;
 if(nvs_open("mv-p4req",NVS_READWRITE,&persistHandle)!=ESP_OK){persistOpen=false;return;}
 persistOpen=true;
 PersistSlot a{},b{};
 const bool hasA=readSlot("req-a",a),hasB=readSlot("req-b",b);
 const PersistSlot* chosen=nullptr;
 if(hasA&&hasB)chosen=a.seq>=b.seq?&a:&b;else if(hasA)chosen=&a;else if(hasB)chosen=&b;
 if(!chosen)return;
 persistSeq=chosen->seq;
 const uint32_t now=millis();
 for(int i=0;i<8;++i){
  const auto& p=chosen->items[i];if(!p.used)continue;
  WriteRecord& w=writes[i];memset(&w,0,sizeof(w));
  strncpy(w.id,p.id,sizeof(w.id)-1);strncpy(w.token,p.token,sizeof(w.token)-1);
  strncpy(w.path,p.path,sizeof(w.path)-1);w.code=p.code;w.used=true;w.at=now;w.boot=0;
 }
 Serial.printf("[wifi-write] restored %u request records\n",(unsigned)persistSeq);
}
bool saveWrites(){
 if(!persistOpen)return false;
 PersistSlot slot{};slot.seq=++persistSeq;
 for(int i=0;i<8;++i){
  const auto& w=writes[i];if(!w.used)continue;
  auto& p=slot.items[i];strncpy(p.id,w.id,sizeof(p.id)-1);strncpy(p.token,w.token,sizeof(p.token)-1);
  strncpy(p.path,w.path,sizeof(p.path)-1);p.code=w.code;p.used=1;
 }
 slot.checksum=checksumOf(slot);
 const char* key=(slot.seq&1)?"req-b":"req-a";
 if(nvs_set_blob(persistHandle,key,&slot,sizeof(slot))!=ESP_OK||nvs_commit(persistHandle)!=ESP_OK){
  Serial.println("[wifi-write] request persistence failed");
  return false;
 }
 return true;
}

const char* reasonFor(int code){
 switch(code){
  case 200:return "OK";case 400:return "Bad Request";case 401:return "Unauthorized";
  case 403:return "Forbidden";case 404:return "Not Found";case 405:return "Method Not Allowed";
  case 409:return "Conflict";case 423:return "Locked";case 429:return "Too Many Requests";
  default:return "Internal Server Error";
 }
}
esp_err_t sendBody(httpd_req_t* r,int code,const char* body){
 char status[48];snprintf(status,sizeof(status),"%d %s",code,reasonFor(code));
 httpd_resp_set_status(r,status);httpd_resp_set_type(r,"application/json");httpd_resp_set_hdr(r,"Connection","close");
 return httpd_resp_sendstr(r,body);
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
String queryValue(const char* query,const char* key){
 if(!query||!*query)return String();
 const String prefix=String(key)+"=";const String text(query);int start=0;
 while(start<(int)text.length()){
  const int end=text.indexOf('&',start);
  const String item=end<0?text.substring(start):text.substring(start,end);
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
 for(int i=0;i<8;++i)if(writes[i].used&&!strcmp(writes[i].id,id)&&!strcmp(writes[i].token,token))return i;
 return -1;
}
int beginWrite(const char* id,const char* token,const char* path,uint32_t now){
 int slot=-1;
 for(int i=0;i<8;++i)if(!writes[i].used){slot=i;break;}
 if(slot<0){slot=0;for(int i=1;i<8;++i)if(uint32_t(now-writes[i].at)>uint32_t(now-writes[slot].at))slot=i;}
 WriteRecord& w=writes[slot];memset(&w,0,sizeof(w));
 strncpy(w.id,id,sizeof(w.id)-1);strncpy(w.token,token,sizeof(w.token)-1);
 strncpy(w.path,path,sizeof(w.path)-1);w.code=0;w.at=now;w.used=true;w.boot=1;
 if(!saveWrites()){writes[slot].used=false;return -1;}
 return slot;
}
void completeWrite(int slot,uint16_t code,const char* body){
 if(slot<0||slot>=8)return;
 writes[slot].code=code;writes[slot].boot=1;strncpy(writes[slot].body,body,sizeof(writes[slot].body)-1);
 saveWrites();
}
esp_err_t handle(httpd_req_t* r){
 const uint32_t now=millis();portENTER_CRITICAL(&mux);idle.touch(httpd_req_to_sockfd(r),now);portEXIT_CRITICAL(&mux);
 String routePath(r->uri);const int queryPosition=routePath.indexOf('?');
 if(queryPosition>=0)routePath=routePath.substring(0,queryPosition);
 const char* path=routePath.c_str();
 const bool statusRoute=!strcmp(path,"/api/status");
 const bool requestRoute=!strncmp(path,"/api/requests/",14);
 const bool writeRoute=!strcmp(path,"/api/auto")||!strcmp(path,"/api/timed")||
   !strcmp(path,"/api/test-fan")||!strcmp(path,"/api/lockoff");
 const bool settingsRoute=!strcmp(path,"/api/settings");
 const bool statsRoute=!strcmp(path,"/api/stats");
 const bool statsRangeRoute=!strcmp(path,"/api/stats/range");
 const bool eventsRoute=!strcmp(path,"/api/events");
 const bool leaseRoute=!strcmp(path,"/api/lease");
 const bool settingsPost=settingsRoute&&r->method==HTTP_POST;
 if(!statusRoute&&!requestRoute&&!writeRoute&&!settingsRoute&&!statsRoute&&!statsRangeRoute&&!eventsRoute&&!leaseRoute)return fail(r,"404 Not Found","NOT_FOUND");
 if(statusRoute&&r->method!=HTTP_GET)return fail(r,"405 Method Not Allowed","METHOD_NOT_ALLOWED");
 if(requestRoute&&r->method!=HTTP_GET)return fail(r,"405 Method Not Allowed","METHOD_NOT_ALLOWED");
 if(writeRoute&&r->method!=HTTP_POST)return fail(r,"405 Method Not Allowed","METHOD_NOT_ALLOWED");
 if((statsRoute||statsRangeRoute||eventsRoute)&&r->method!=HTTP_GET)return fail(r,"405 Method Not Allowed","METHOD_NOT_ALLOWED");
 if(settingsRoute&&r->method!=HTTP_GET&&r->method!=HTTP_POST)return fail(r,"405 Method Not Allowed","METHOD_NOT_ALLOWED");
 if(leaseRoute&&r->method!=HTTP_POST)return fail(r,"405 Method Not Allowed","METHOD_NOT_ALLOWED");
 if(statusRoute&&(r->content_len||httpd_req_get_hdr_value_len(r,"Transfer-Encoding")))return fail(r,"400 Bad Request","BODY_NOT_ALLOWED");
 if(requestRoute&&(r->content_len||httpd_req_get_hdr_value_len(r,"Transfer-Encoding")))return fail(r,"400 Bad Request","BODY_NOT_ALLOWED");
 if((writeRoute||settingsPost||leaseRoute)&&(r->content_len==0||r->content_len>256||httpd_req_get_hdr_value_len(r,"Transfer-Encoding")))return fail(r,"400 Bad Request","INVALID_BODY");
 if(!settingsPost&&(settingsRoute||statsRangeRoute||eventsRoute)&&(r->content_len||httpd_req_get_hdr_value_len(r,"Transfer-Encoding")))return fail(r,"400 Bad Request","BODY_NOT_ALLOWED");
 if(!wifiBindingReady())return fail(r,"503 Service Unavailable","RESOURCE_BUSY");
 char authorization[128]{};size_t n=httpd_req_get_hdr_value_len(r,"Authorization");
 if(n!=50||httpd_req_get_hdr_value_str(r,"Authorization",authorization,sizeof(authorization))!=ESP_OK||strncmp(authorization,"Bearer ",7))
  return fail(r,"401 Unauthorized","AUTH_REQUIRED");
 const auto auth=wifiBindingAuthorize(authorization+7,now);memset(authorization,0,sizeof(authorization));
 if(!auth.valid)return fail(r,"401 Unauthorized","BINDING_INVALID");
 if(leaseRoute){
  char body[257]{};const size_t length=r->content_len;
  if(httpd_req_recv(r,body,length)!=length)return fail(r,"400 Bad Request","INVALID_BODY");
  body[length]='\0';
  if(formValue(String(body),"action")=="release")leaseRelease(auth.tokenId);
  return sendBody(r,200,"{\"released\":true}");
 }
 // 待绑定 token 只允许读取状态来完成 HTTPS 证书验证；不占用 WiFi 租约，
 // 这样第二台手机可以在第一台 WiFi 主控仍在线时完成绑定。
 if(!(auth.pending&&statusRoute)&&!leaseAllowed(auth.tokenId,now))return fail(r,"423 Locked","WIFI_IN_USE");
 if(!rateAllowed(auth.tokenId,now))return fail(r,"429 Too Many Requests","RATE_LIMITED");
 if((settingsRoute||statsRoute||statsRangeRoute||eventsRoute)&&!settingsPost){
  char query[160]{};const bool hasQuery=httpd_req_get_url_query_str(r,query,sizeof(query))==ESP_OK;
  ControlApiRequest request;request.method="GET";request.path=path;
  const String profile=hasQuery?queryValue(query,"profile"):String();
  if(profile.length()&&!settingsStoreParseProfile(profile,&request.profile))return fail(r,"400 Bad Request","INVALID_PROFILE");
  if(statsRoute){
   request.mode=hasQuery?queryValue(query,"mode"):String("today");
   request.eventOffset=hasQuery?queryValue(query,"offset"):String();
  }
  if(statsRangeRoute){
   request.rangeFrom=queryValue(query,"from");
   request.rangeTo=queryValue(query,"to");
   request.rangeTz=queryValue(query,"tzOffsetSeconds");
  }
  if(eventsRoute){
   request.eventStart=queryValue(query,"start");
   request.eventEnd=queryValue(query,"end");
   request.eventOffset=queryValue(query,"offset");
   request.eventLimit=queryValue(query,"limit");
   request.eventBeforeId=queryValue(query,"beforeId");
   request.eventDataEpoch=queryValue(query,"dataEpoch");
   request.eventKind=queryValue(query,"kind");
  }
  const auto result=controlApiDispatch(request);
  return sendBody(r,result.statusCode,result.body.c_str());
 }
 if(requestRoute){
  const char* id=path+14;if(!uuidValid(id))return fail(r,"400 Bad Request","INVALID_REQUEST_ID");
  const int index=findWrite(id,auth.tokenId);
  if(index<0||uint32_t(now-writes[index].at)>600000UL)return fail(r,"404 Not Found","UNKNOWN");
  char out[160];snprintf(out,sizeof(out),"{\"requestId\":\"%s\",\"status\":\"%s\",\"httpStatus\":%u}",
    id,writes[index].code==0?(writes[index].boot?"pending":"unknown"):(writes[index].code<400?"applied":"rejected"),writes[index].code);
  return sendBody(r,200,out);
 }
 if(writeRoute||settingsPost){
  char requestId[64]{};n=httpd_req_get_hdr_value_len(r,"X-Request-Id");
  if(n!=36||httpd_req_get_hdr_value_str(r,"X-Request-Id",requestId,sizeof(requestId))!=ESP_OK||!uuidValid(requestId))
   return fail(r,"400 Bad Request","REQUEST_ID_REQUIRED");
  const int existing=findWrite(requestId,auth.tokenId);
  if(existing>=0){
   if(strcmp(writes[existing].path,path))return fail(r,"409 Conflict","REQUEST_ID_CONFLICT");
   if(writes[existing].code==0)
    return fail(r,"409 Conflict",writes[existing].boot?"REQUEST_IN_PROGRESS":"REQUEST_UNKNOWN");
   if(writes[existing].code<400)return sendBody(r,200,"{\"ok\":true,\"duplicate\":true}");
   char duplicate[96];snprintf(duplicate,sizeof(duplicate),"{\"error\":\"DUPLICATE\",\"httpStatus\":%u}",writes[existing].code);
   return sendBody(r,writes[existing].code,duplicate);
  }
  char body[257]{};const size_t length=r->content_len;
  if(httpd_req_recv(r,body,length)!=length)return fail(r,"400 Bad Request","INVALID_BODY");
  body[length]='\0';
  if(!auth.valid||auth.pending)return fail(r,"401 Unauthorized","BINDING_NOT_ACTIVE");
  if(otaManagerIsActive())return fail(r,"423 Locked","OTA_BUSY");
  ControlApiRequest request;request.method="POST";request.path=path;request.body=body;
  const String profile=formValue(String(body),"profile");
  if(profile.length()&&!settingsStoreParseProfile(profile,&request.profile))return fail(r,"400 Bad Request","INVALID_PROFILE");
  const String modeText=formValue(String(body),"mode");
  const String stageText=formValue(String(body),"stage");
  request.mode=modeText;request.stage=stageText;
  request.replace=formValue(String(body),"replace")=="1";
  request.switchProfile=formValue(String(body),"switchProfile")=="1";
  if(settingsPost){
   request.hasSettings=true;
   const bool ok=controlApiParseTimeValue(formValue(String(body),"startupCooldownMs"),false,&request.settings.startupCooldownMs)&&
     controlApiParseTimeValue(formValue(String(body),"leaveConfirmMs"),true,&request.settings.leaveConfirmMs)&&
     controlApiParseTimeValue(formValue(String(body),"autoExhaustMs"),false,&request.settings.autoExhaustMs)&&
     controlApiParseTimeValue(formValue(String(body),"manualExhaustMs"),false,&request.settings.manualExhaustMs)&&
     controlApiParseTimeValue(formValue(String(body),"pauseMs"),false,&request.settings.pauseMs);
   if(!ok)return fail(r,"400 Bad Request","INVALID_SETTINGS");
  }
  const int pendingSlot=beginWrite(requestId,auth.tokenId,path,now);
  if(pendingSlot<0)return fail(r,"503 Service Unavailable","DEDUP_UNAVAILABLE");
  const auto result=controlApiDispatch(request);
  completeWrite(pendingSlot,result.statusCode,result.body.c_str());
  return sendBody(r,result.statusCode,result.body.c_str());
 }
 char body[2048];size_t length=0;uint32_t age=0,seq=0;bool busy=false;
 portENTER_CRITICAL(&mux);
 busy=otaSuspended;age=now-sampledAt;seq=sampleSeq;length=snapshotLength;
 if(length&&length<sizeof(body))memcpy(body,snapshot,length);
 portEXIT_CRITICAL(&mux);
 if(busy)return fail(r,"503 Service Unavailable","OTA_BUSY");
 if(!length||age>3000)return fail(r,"503 Service Unavailable","STATUS_STALE");
 if(length>=sizeof(body)||body[length-1]!='}')return fail(r,"503 Service Unavailable","RESOURCE_BUSY");
 const int added=snprintf(body+length-1,sizeof(body)-length+1,
  ",\"deviceId\":\"%s\",\"tokenId\":\"%s\",\"bindingState\":\"%s\",\"protocolVersion\":1,\"sampleSeq\":%lu,\"sampleAgeMs\":%lu}",
  wifiDeviceId(),auth.tokenId,auth.pending?"pending":"active",(unsigned long)seq,(unsigned long)age);
 if(added<0||size_t(added)>=sizeof(body)-length+1)return fail(r,"503 Service Unavailable","RESOURCE_BUSY");
 if(auth.pending&&!wifiBindingMarkVerified(auth.tokenId))return fail(r,"503 Service Unavailable","RESOURCE_BUSY");
 httpd_resp_set_type(r,"application/json");httpd_resp_set_hdr(r,"Connection","close");
 return httpd_resp_send(r,body,length-1+added);
}
void tlsEvent(esp_https_server_user_cb_arg_t* event){
 int fd=-1;if(esp_tls_get_conn_sockfd(event->tls,&fd)!=ESP_OK)return;
 portENTER_CRITICAL(&mux);
 if(event->user_cb_state==HTTPD_SSL_USER_CB_SESS_CREATE)idle.opened(fd,millis());else idle.closed(fd);
 portEXIT_CRITICAL(&mux);
 if(event->user_cb_state==HTTPD_SSL_USER_CB_SESS_CREATE)
  Serial.printf("[wifi-read] https-stack-free=%u\n",uxTaskGetStackHighWaterMark(nullptr));
}
void closeIdle(void*){
 portENTER_CRITICAL(&mux);bool valid=idle.valid(idleWork,millis(),15000);int fd=idleWork.socket;portEXIT_CRITICAL(&mux);
 if(valid)shutdown(fd,SHUT_RDWR);
 portENTER_CRITICAL(&mux);idle.completed();portEXIT_CRITICAL(&mux);
}
void resetLease(){leaseToken[0]='\0';leaseTouchedAt=0;}
void stop(){if(server){httpd_ssl_stop(server);server=nullptr;portENTER_CRITICAL(&mux);idle=WifiTlsIdleTracker();portEXIT_CRITICAL(&mux);} }
void start(){
 loadWrites();
 const auto caps=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
 // schema 5 的常驻统计缓冲（daily[30]+events[512]≈19.6KB）把空闲堆压到 ~50KB，
 // 原来的 56 KiB 门槛让 WiFi 服务永远起不来（串口 ready=0 resource gate blocked）。
 // TLS 峰值约 38 KiB，真正的硬约束是最大连续块，空闲堆作为次要下限。
 if(heap_caps_get_free_size(caps)<48*1024||heap_caps_get_largest_free_block(caps)<20*1024){Serial.println("[wifi-read] resource gate blocked");return;}
 if(heap_caps_get_free_size(caps)<56*1024){Serial.printf("[wifi-read] low heap pass: %u\n",heap_caps_get_free_size(caps));}
 // 栈从 16KB 降到 10KB：实测 TLS 握手期间 heap 只剩 6KB、最大块 1.5KB，
 // 16KB 的 httpd 栈本身就会挤占握手缓冲，导致 mbedtls 分配失败。
 httpd_ssl_config_t c=HTTPD_SSL_CONFIG_DEFAULT();c.httpd.stack_size=10240;c.httpd.max_open_sockets=1;c.httpd.backlog_conn=1;c.httpd.lru_purge_enable=true;
 c.httpd.recv_wait_timeout=10;c.httpd.send_wait_timeout=10;c.httpd.uri_match_fn=httpd_uri_match_wildcard;
 c.port_secure=8443;c.tls_handshake_timeout_ms=10000;
 c.servercert=wifiDeviceCertificate(&c.servercert_len);c.prvtkey_pem=wifiDevicePrivateKey(&c.prvtkey_len);c.user_cb=tlsEvent;
 if(httpd_ssl_start(&server,&c)!=ESP_OK){server=nullptr;return;}
 httpd_uri_t route{};route.uri="/*";route.method=static_cast<httpd_method_t>(HTTP_ANY);route.handler=handle;
 if(httpd_register_uri_handler(server,&route)!=ESP_OK){stop();return;}
 Serial.println("[wifi-read] HTTPS ready on 8443; GET /api/status only");
}
}
void wifiHttpsResetLease(){resetLease();}
bool wifiHttpsServerReady(){return server!=nullptr;}
void wifiHttpsServerSuspendForOta(){portENTER_CRITICAL(&mux);otaSuspended=true;portEXIT_CRITICAL(&mux);stop();}
void wifiHttpsServerUpdate(){
 wifiBindingUpdate();const auto now=millis();
 if(otaManagerIsActive()||WiFi.status()!=WL_CONNECTED){wifiHttpsServerSuspendForOta();return;}
 portENTER_CRITICAL(&mux);otaSuspended=false;portEXIT_CRITICAL(&mux);
 if(!wifiDeviceIdentityReady()||!wifiBindingReady()){stop();return;}
 if(!server&&uint32_t(now-lastStart)>=5000){lastStart=now;start();}
 if(!sampleSeq||uint32_t(now-sampledAt)>=250){
  ControlApiRequest request;request.method="GET";request.path="/api/status";const auto result=controlApiDispatch(request);
  portENTER_CRITICAL(&mux);
  if(result.statusCode==200&&result.body.length()<sizeof(snapshot)){
   snapshotLength=result.body.length();memcpy(snapshot,result.body.c_str(),snapshotLength);sampledAt=now;++sampleSeq;
  }else snapshotLength=0;
  portEXIT_CRITICAL(&mux);
 }
 if(server){
  portENTER_CRITICAL(&mux);bool scheduled=idle.schedule(now,15000,&idleWork);portEXIT_CRITICAL(&mux);
  if(scheduled&&httpd_queue_work(server,closeIdle,nullptr)!=ESP_OK){portENTER_CRITICAL(&mux);idle.completed();portEXIT_CRITICAL(&mux);}
 }
 if(now-lastLog>=10000){lastLog=now;const auto caps=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
  Serial.printf("[wifi-read] ready=%d heap=%u largest=%u minimum=%u loop-stack-free=%u\n",server!=nullptr,heap_caps_get_free_size(caps),heap_caps_get_largest_free_block(caps),heap_caps_get_minimum_free_size(caps),uxTaskGetStackHighWaterMark(nullptr));}
}
#endif
