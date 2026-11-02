from pathlib import Path
import shutil,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class StorageBudgetTest(unittest.TestCase):
 def test_observed_full_state_rejected_and_cleanup_state_allowed(self):
  with tempfile.TemporaryDirectory() as d:
   p=Path(d);(p/'main.cpp').write_text('''#include "WifiStorageBudget.h"
#include <assert.h>
int main(){using namespace WifiStorageBudget;
assert(!allowsBinding(211,false));assert(!allowsBinding(136,true));
assert(allowsBinding(337,false));assert(allowsBinding(262,true));
assert(allowsBinding(required(false),false));assert(!allowsBinding(required(false)-1,false));
}''')
   subprocess.run([shutil.which('clang++')or'c++','-std=c++11','-I',str(ROOT),str(p/'main.cpp'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
