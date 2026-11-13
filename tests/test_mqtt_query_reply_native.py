"""执行实际分片发送函数，覆盖同步回调、锁顺序与发送失败恢复。"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class QueryReplySenderTest(unittest.TestCase):
    def test_enqueue_runs_without_query_lock_and_preserves_callback_state(self):
        source = (ROOT / 'WifiMqttTransport.cpp').read_text()
        sender = source[source.index('void updateQueryReply() {'):source.index('void handleQuery(', source.index('void updateQueryReply() {'))]
        harness = r'''
#include <cassert>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
using String=std::string;
struct Body:std::string { using std::string::operator=;
 String substring(size_t a,size_t b){return substr(a,b-a);} } pendingQueryBody;
constexpr int pdTRUE=1; int mutexObject;void* queryReplyMutex=&mutexObject;
bool locked=false;uint32_t nowMs=100;
int xSemaphoreTake(void*,int){if(locked)return 0;locked=true;return pdTRUE;}
void xSemaphoreGive(void*){assert(locked);locked=false;}
int pdMS_TO_TICKS(int x){return x;}
uint32_t millis(){return nowMs;}
struct Logger {template<class... T> void printf(const char*,T...){} } Serial;
constexpr uint32_t kQueryReplyTimeoutMs=30000,kQueryAckTimeoutMs=1200,kQueryChunkPacingMs=20;
bool pendingQueryReply=true,pendingQueryAwaitingAck=false;
uint32_t pendingQueryStartedAt=0,pendingQueryLastActionAt=0;
uint16_t pendingQueryChunkIndex=0,pendingQueryChunkCount=1,pendingQueryStatus=200;
unsigned pendingQueryAckRetries=0,pendingQueryPublishFailures=0;
size_t pendingQueryOffsets[2]={0,7};char pendingQueryId[37]="old-query";
std::atomic<int32_t> acknowledgedQueryChunk{-1};
void resetPendingQueryReply(){pendingQueryReply=false;pendingQueryAwaitingAck=false;acknowledgedQueryChunk=-1;}
#define strlcpy test_strlcpy
void test_strlcpy(char* out,const char* in,size_t n){std::snprintf(out,n,"%s",in);}
int mode=0,calls=0;
bool publishQueryChunk(const char*,uint16_t,uint16_t,uint16_t,const String& part){
 assert(!locked);assert(pendingQueryAwaitingAck);assert(part=="payload");++calls;
 if(mode==1) resetPendingQueryReply();
 if(mode==2){resetPendingQueryReply();pendingQueryReply=true;strlcpy(pendingQueryId,"new-query",37);pendingQueryPublishFailures=7;}
 return mode!=3;
}
''' + sender + r'''
void prepare(int selectedMode){
 assert(!locked);mode=selectedMode;pendingQueryBody="payload";
 pendingQueryReply=true;pendingQueryAwaitingAck=false;pendingQueryChunkIndex=0;
 pendingQueryStartedAt=0;pendingQueryLastActionAt=0;pendingQueryAckRetries=0;
 pendingQueryPublishFailures=0;acknowledgedQueryChunk=-1;strlcpy(pendingQueryId,"old-query",37);
}
int main(){
 prepare(0);updateQueryReply();assert(!locked&&pendingQueryAwaitingAck);
 prepare(1);updateQueryReply();assert(!locked&&!pendingQueryReply);
 prepare(2);updateQueryReply();assert(!locked&&pendingQueryReply&&!pendingQueryAwaitingAck);
 assert(!strcmp(pendingQueryId,"new-query")&&pendingQueryPublishFailures==7);
 prepare(3);updateQueryReply();assert(!locked&&!pendingQueryAwaitingAck&&pendingQueryPublishFailures==1);
 nowMs+=20;mode=0;updateQueryReply();assert(!locked&&pendingQueryAwaitingAck&&pendingQueryPublishFailures==0);
}
'''
        compiler = shutil.which('clang++') or shutil.which('c++')
        if not compiler:
            self.skipTest('需要 C++ 编译器')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'test.cpp').write_text(harness)
            subprocess.run([compiler, '-std=c++17', '-fsanitize=address,undefined',
                            str(root / 'test.cpp'), '-o', str(root / 'test')], check=True)
            subprocess.run([str(root / 'test')], check=True)
