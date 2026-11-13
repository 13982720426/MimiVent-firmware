"""验证实际发送函数的总预算、状态预算、SDK 入队错误和恢复路径。"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MqttOutboxTest(unittest.TestCase):
    def test_bounds_and_enqueue_failures(self):
        source = (ROOT / 'WifiMqttTransport.cpp').read_text()
        self.assertIn('mqtt.outbox.limit = kOutboxLimitBytes;', source)
        constants = '\n'.join(re.findall(
            r'constexpr size_t k(?:MaxMessage|OutboxLimitBytes|TelemetryBudgetBytes) = [^;]+;',
            source,
        ))
        publish = source[source.index('bool publishText('):source.index('bool publishAck(')]
        harness = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <string>
void* client=(void*)1;bool connected=true;
std::atomic<uint32_t> queueRejected{0},enqueueFailures{0};
int queued=0,result=1,calls=0;
int esp_mqtt_client_get_outbox_size(void*){return queued;}
int esp_mqtt_client_enqueue(void*,const char*,const char*,int,int,bool,bool){++calls;return result;}
''' + constants + publish + r'''
int main(){
 assert(kTelemetryBudgetBytes<kOutboxLimitBytes);
 assert(publishText("state","{}",1,true));assert(calls==1);
 std::string maxState(kMaxMessage,'x');calls=0;
 assert(publishText("mimivent/v1/0123456789abcdef0123456789abcdef/state",maxState.c_str(),1,true));
 assert(calls==1);
 queued=kTelemetryBudgetBytes;calls=0;
 assert(!publishText("state","{}",1,true));assert(calls==0&&queueRejected==1);
 assert(publishText("ack","{}",1,false));assert(calls==1);
 queued=kOutboxLimitBytes;calls=0;
 assert(!publishText("ack","{}",1,false));assert(calls==0);
 queued=50000;assert(!publishText("ack","{}",1,false));assert(calls==0);
 queued=0;result=-2;assert(!publishText("ack","{}",1,false));assert(enqueueFailures==1);
 result=-1;assert(!publishText("ack","{}",1,false));assert(enqueueFailures==2);
 result=1;assert(publishText("ack","{}",1,false));
 std::string large(kOutboxLimitBytes,'x');calls=0;
 assert(!publishText("ack",large.c_str(),1,false));assert(calls==0);
 connected=false;assert(!publishText("ack","{}",1,false));assert(calls==0);
}
'''
        compiler = shutil.which('clang++') or shutil.which('c++')
        if not compiler:
            self.skipTest('需要 C++ 编译器')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.cpp').write_text(harness)
            subprocess.run([compiler, '-std=c++17', '-fsanitize=address,undefined',
                            str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)
