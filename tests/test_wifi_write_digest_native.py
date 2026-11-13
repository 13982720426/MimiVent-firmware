"""验证实际去重记录的正文摘要、重启恢复和两个旧版布局迁移。"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class WriteDigestTest(unittest.TestCase):
    def test_digest_persistence_legacy_and_rollback(self):
        source = (ROOT / 'WifiControlService.cpp').read_text()
        records = source[source.index('struct WriteRecord {'):source.index('String formValue(')]
        lookup = source[source.index('int findWrite('):source.index('int beginWrite(')]
        writes = source[source.index('int beginWrite('):source.index('\n}\n\nvoid wifiControlServiceBegin()')]
        harness = r'''
#include "Arduino.h"
#include "SPIFFS.h"
#include <cassert>
#include <map>
#include <vector>
#include <memory>
#include <new>
using nvs_handle_t=int;
using esp_err_t=int;
const char* esp_err_to_name(int code){return code==0?"ESP_OK":"MOCK_FAILURE";}
constexpr int ESP_OK=0,NVS_READWRITE=1;
std::map<std::string,std::vector<uint8_t>> blobs;
bool failCommit=false;
int nvs_open(const char*,int,nvs_handle_t* out){*out=1;return 0;}
int nvs_get_blob(int,const char* key,void* out,size_t* length){
 auto found=blobs.find(key);if(found==blobs.end())return -1;
 const auto& bytes=found->second;
 if(!out){*length=bytes.size();return 0;}
 if(*length<bytes.size())return -1;
 memcpy(out,bytes.data(),bytes.size());*length=bytes.size();return 0;
}
int nvs_set_blob(int,const char* key,const void* data,size_t length){
 if(failCommit)return -1;
 const auto* bytes=static_cast<const uint8_t*>(data);blobs[key]={bytes,bytes+length};return 0;
}
int nvs_commit(int){return failCommit?-1:0;}
int nvs_erase_key(int,const char* key){blobs.erase(key);return 0;}
void nvs_close(int){}
''' + records + lookup + writes + r'''
const char* phone="0123456789abcdef0123456789abcdef";
const char* firstId="00000001-ABCD-4000-8000-000000000ABC";
const char* secondId="00000002-0000-4000-8000-000000000000";
void reboot(){
 memset(writes,0,sizeof(writes));persistLoaded=false;persistOpen=false;persistSeq=0;
 loadWrites();
}
template<size_t N> void installLegacy(){
 LegacyPersistSlot<N> old{};old.seq=19;
 auto& record=old.items[0];strcpy(record.id,firstId);strcpy(record.token,phone);
 strcpy(record.path,"/api/timed");record.code=200;record.used=1;
 uint32_t hash=2166136261u;auto* bytes=reinterpret_cast<uint8_t*>(&old);
 for(size_t i=0;i<sizeof(old);++i){hash^=bytes[i];hash*=16777619u;}
 old.checksum=hash;blobs.clear();files.clear();blobs["req-a"]={bytes,bytes+sizeof(old)};
}
int main(){
 uint8_t onHash[32]{},offHash[32]{};onHash[0]=1;offHash[0]=2;
 reboot();int slot=beginWrite(firstId,phone,"/api/timed",onHash,100);
 assert(slot==0);completeWrite(slot,200,"{}");
 assert(writeConflict(writes[0],"/api/timed",onHash)==nullptr);
 assert(!strcmp(writeConflict(writes[0],"/api/timed",offHash),"REQUEST_ID_CONFLICT"));
 assert(!strcmp(writeConflict(writes[0],"/api/auto",onHash),"REQUEST_ID_CONFLICT"));
 reboot();assert(writes[0].hasBodyHash&&writes[0].code==200);
 assert(findWrite(firstId,phone)==0);
 assert(!strcmp(writes[0].id,"00000001-abcd-4000-8000-000000000abc"));
 assert(writeConflict(writes[0],"/api/timed",onHash)==nullptr);
 assert(!strcmp(writeConflict(writes[0],"/api/timed",offHash),"REQUEST_ID_CONFLICT"));
 PersistSlot damaged{};assert(readSlot("req-a",damaged)||readSlot("req-b",damaged));
 auto saved=files;for(auto& item:files)item.second.back()^=1;
 assert(!readSlot("req-a",damaged)&&!readSlot("req-b",damaged));files=saved;
 installLegacy<8>();reboot();assert(writes[0].used&&writes[0].code==200&&!writes[0].hasBodyHash);
 assert(!strcmp(writeConflict(writes[0],"/api/timed",onHash),"REQUEST_BODY_UNKNOWN"));
 failCommit=true;assert(saveWrites());failCommit=false;
 assert(blobs.empty());reboot();assert(writes[0].used&&!writes[0].hasBodyHash);
 installLegacy<32>();reboot();assert(writes[0].used&&writes[0].code==200&&!writes[0].hasBodyHash);
 slot=beginWrite(secondId,phone,"/api/timed",onHash,100);assert(slot==1);
 completeWrite(slot,200,"{}");reboot();
 assert(!writes[0].hasBodyHash&&writes[1].hasBodyHash);
 assert(writeConflict(writes[1],"/api/timed",onHash)==nullptr);
 for(size_t i=2;i<kWriteCapacity;++i){char id[37];snprintf(id,sizeof(id),"%08x-0000-4000-8000-000000000000",unsigned(i+10));
  assert(beginWrite(id,phone,"/api/timed",onHash,100)==int(i));}
 const char* nextId="ffffffff-0000-4000-8000-000000000000";
 assert(beginWrite(nextId,phone,"/api/timed",offHash,100)==-2);
 const auto previous=writes[0];spiffsWriteFailure=true;
 assert(beginWrite(nextId,phone,"/api/timed",offHash,700000)==-1);
 assert(!memcmp(&previous,&writes[0],sizeof(previous)));spiffsWriteFailure=false;
 assert(beginWrite(nextId,phone,"/api/timed",offHash,700000)==0);
 assert(writeConflict(writes[0],"/api/timed",offHash)==nullptr);
 uint8_t packed[16];char text[37];
 assert(packId(firstId,packed,true));unpackId(packed,text,true);
 assert(!strcmp(text,"00000001-abcd-4000-8000-000000000abc"));
 assert(!packId("bad",packed,true));
 assert(sizeof(PersistSlot)<=sizeof(LegacyPersistSlot<32>));
}
'''
        compiler = shutil.which('clang++') or shutil.which('c++')
        if not compiler:
            self.skipTest('需要 C++ 编译器')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.cpp').write_text(harness)
            subprocess.run([compiler, '-std=c++17', '-fsanitize=address,undefined',
                            '-I', str(ROOT / 'tests/native_support'), str(path / 'test.cpp'),
                            '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)
