#include "WifiLegacyStorage.h"
#if MIMIVENT_ENABLE_WIFI && MIMIVENT_WIFI_READ_CONTROL
#include "WifiDeviceIdentity.h"
#include "OtaManager.h"
#include <nvs.h>
namespace {
const char* keys[]={"identity","identity_v2"};
const size_t expectedBytes[]={1928,1936};
const char* text(cJSON* root,const char* name){auto* v=cJSON_GetObjectItemCaseSensitive(root,name);return cJSON_IsString(v)?v->valuestring:"";}
String finish(cJSON* root){char* raw=cJSON_PrintUnformatted(root);String value=raw?raw:"{\"error\":\"MEMORY\"}";cJSON_free(raw);cJSON_Delete(root);return value;}
String failure(const char* code){auto* root=cJSON_CreateObject();cJSON_AddStringToObject(root,"error",code);return finish(root);}
}
String wifiLegacyStorageRequest(cJSON* request,const char* owner){
 if(!owner||strlen(owner)!=14)return failure("PAIRING_REQUIRED");
 if(otaManagerIsActive())return failure("OTA_BUSY");
 const bool erase=!strcmp(text(request,"op"),"wifi.storage.cleanupLegacy");
 if(erase&&(!wifiDeviceIdentityReady()||strcmp(text(request,"expectedDeviceId"),wifiDeviceId())||
      strcmp(text(request,"expectedCertPin"),wifiDevicePin())||strcmp(text(request,"confirm"),"DELETE_LEGACY_WIFI_IDENTITIES")))
  return failure("CONFIRMATION_REQUIRED");
 nvs_handle_t handle;const auto opened=nvs_open("mv-wifi-v1",erase?NVS_READWRITE:NVS_READONLY,&handle);
 if(opened!=ESP_OK)return failure("STORAGE_UNAVAILABLE");
 size_t lengths[2]{};
 for(size_t i=0;i<2;++i){auto rc=nvs_get_blob(handle,keys[i],nullptr,&lengths[i]);
  if(rc!=ESP_OK&&rc!=ESP_ERR_NVS_NOT_FOUND){nvs_close(handle);return failure("STORAGE_UNAVAILABLE");}
  if(erase&&lengths[i]&&lengths[i]!=expectedBytes[i]){nvs_close(handle);return failure("LEGACY_FORMAT_UNKNOWN");}
 }
 if(erase){
  // 白名单仅这两个未被新实现使用的旧实验身份；不清 namespace，不触碰 credentials。
  for(size_t i=0;i<2;++i)if(lengths[i]&&nvs_erase_key(handle,keys[i])!=ESP_OK){nvs_close(handle);return failure("CLEANUP_UNCONFIRMED");}
  if(nvs_commit(handle)!=ESP_OK){nvs_close(handle);return failure("CLEANUP_UNCONFIRMED");}
 }
 nvs_close(handle);auto* root=cJSON_CreateObject();auto* list=cJSON_AddArrayToObject(root,"legacyIdentityKeys");
 for(size_t i=0;i<2;++i){auto* item=cJSON_CreateObject();cJSON_AddStringToObject(item,"key",keys[i]);cJSON_AddNumberToObject(item,"bytes",lengths[i]);cJSON_AddItemToArray(list,item);}
 cJSON_AddBoolToObject(root,"removed",erase);cJSON_AddStringToObject(root,"deviceId",wifiDeviceId());
 cJSON_AddStringToObject(root,"certPinSha256",wifiDevicePin());
 nvs_stats_t stats{};if(nvs_get_stats(nullptr,&stats)==ESP_OK)cJSON_AddNumberToObject(root,"nvsFreeEntries",stats.free_entries);
 return finish(root);
}
#endif
