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
 auto other=entry("third","owner-a","33333333333333333333333333333333",3);
 assert(!strcmp(c.begin(other,"c","t",false,240005),"BINDINGS_FULL"));
 auto replacement=entry("phone-a","owner-a","44444444444444444444444444444444",4);
 assert(c.begin(replacement,"r","t",false,240006));assert(!c.begin(replacement,"r","t",true,240006));
 assert(c.active.entries[0].hash[0]==1);c.pending.verified=true;fail=true;
 assert(c.commit("owner-a",replacement.id,240007,save,nullptr));assert(c.active.entries[0].hash[0]==1);
 fail=false;assert(!c.commit("owner-a",replacement.id,240008,save,nullptr));
 assert(c.active.entries[0].hash[0]==4);assert(valid(c.active));
 auto damaged=c.active;damaged.entries[0].hash[0]^=1;assert(!valid(damaged));
 assert(!c.revoke(replacement.id,save,nullptr));n=writes;assert(!c.revoke(replacement.id,save,nullptr)&&writes==n);
 Core wrap;assert(!wrap.open("owner-a",UINT32_MAX-100));
 assert(!wrap.begin(a,"r","t",false,UINT32_MAX-90));wrap.tick(120000);assert(!wrap.pending.exists&&!wrap.window);
}
'''
  with tempfile.TemporaryDirectory() as d:
   p=pathlib.Path(d);(p/'main.cpp').write_text(source)
   subprocess.run([shutil.which('clang++') or 'c++','-std=c++11','-fsanitize=address,undefined','-I',str(ROOT),str(p/'main.cpp'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
