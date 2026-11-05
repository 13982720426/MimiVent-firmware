import pathlib, shutil, subprocess, tempfile, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]

class BindingsTest(unittest.TestCase):
 def test_transactions_owner_expiry_replacement_and_failure(self):
  source=r'''
#include "WifiBindingCore.h"
#include <assert.h>
using namespace WifiBindings;
bool fail=false; int writes=0;
bool save(const Record& r,void*) { ++writes; assert(valid(r)); return !fail; }
Entry entry(const char* client,const char* owner,const char* id,int hash) {
 Entry e{};e.used=1;strcpy(e.client,client);strcpy(e.owner,owner);strcpy(e.id,id);memset(e.hash,hash,32);return e;
}
int main(){
 Core c; auto a=entry("phone-a","owner-a","11111111111111111111111111111111",1);
 auto b=entry("phone-b","owner-b","22222222222222222222222222222222",2);
 assert(c.begin(a,"req","token",false,0)); assert(!c.open("owner-a",0));
 assert(!c.begin(a,"req","token",false,1)); assert(!c.begin(a,"req","different-token",false,2));
 assert(!strcmp(c.pending.token,"token")); assert(c.open("owner-b",3));
 assert(!strcmp(c.status("owner-b",a.id,3),"forbidden"));
 assert(c.cancel("owner-b",a.id)); assert(c.commit("owner-a",a.id,4,save,nullptr));
 c.pending.verified=true; fail=true;assert(c.commit("owner-a",a.id,5,save,nullptr));
 assert(!c.active.entries[0].used && c.pending.exists);fail=false;
 assert(!c.commit("owner-a",a.id,6,save,nullptr)); int n=writes;
 assert(!c.commit("owner-a",a.id,7,save,nullptr)&&writes==n);
 c.tick(120001); assert(!c.open("owner-b",120002)); assert(!c.begin(b,"b","token-b",false,120003));
 c.pending.verified=true; assert(!c.commit("owner-b",b.id,120004,save,nullptr));
 c.tick(240003); assert(!c.open("owner-a",240004));
 auto third=entry("third","owner-c","33333333333333333333333333333333",3);
 assert(c.open("owner-c",240006)); // owner-a 的窗口尚未过期，应拒绝其他 owner。
 c.tick(360005);
 assert(!c.open("owner-c",360006));assert(!c.begin(third,"c","t",false,360007));
 c.pending.verified=true;assert(!c.commit("owner-c",third.id,360008,save,nullptr));
 auto fourth=entry("fourth","owner-a","44444444444444444444444444444444",4);
 c.tick(480007);
 assert(!c.open("owner-a",480009));assert(!strcmp(c.begin(fourth,"d","t",false,480010),"BINDINGS_FULL"));
 auto replacement=entry("phone-a","owner-a","55555555555555555555555555555555",5);
 assert(c.begin(replacement,"r","t",false,480011));assert(!c.begin(replacement,"r","t",true,480011));
 assert(c.active.entries[0].hash[0]==1);c.pending.verified=true;fail=true;
 assert(c.commit("owner-a",replacement.id,480012,save,nullptr));assert(c.active.entries[0].hash[0]==1);
 fail=false;assert(!c.commit("owner-a",replacement.id,480013,save,nullptr));
 assert(c.active.entries[0].hash[0]==5);assert(valid(c.active));
 auto damaged=c.active;damaged.entries[0].hash[0]^=1;assert(!valid(damaged));
 assert(!c.revoke(replacement.id,save,nullptr));n=writes;assert(!c.revoke(replacement.id,save,nullptr)&&writes==n);
 RecordV1 legacy{};legacy.magic=0x3142564d;legacy.version=1;legacy.bytes=sizeof(legacy);legacy.sequence=9;
 legacy.entries[0]=a;legacy.entries[1]=b;
 legacy.checksum=crc(reinterpret_cast<const uint8_t*>(&legacy),offsetof(RecordV1,checksum));
 assert(validV1(legacy));auto migrated=migrateV1(legacy);assert(valid(migrated));
 assert(migrated.entries[0].used&&migrated.entries[1].used&&!migrated.entries[2].used);
 Core wrap;assert(!wrap.open("owner-a",UINT32_MAX-100));
 assert(!wrap.begin(a,"r","t",false,UINT32_MAX-90));wrap.tick(120000);assert(!wrap.pending.exists&&!wrap.window);
}
'''
  with tempfile.TemporaryDirectory() as d:
   p=pathlib.Path(d);(p/'main.cpp').write_text(source)
   subprocess.run([shutil.which('clang++') or 'c++','-std=c++11','-fsanitize=address,undefined','-I',str(ROOT),str(p/'main.cpp'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
