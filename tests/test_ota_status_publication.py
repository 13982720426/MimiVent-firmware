"""执行固件实际控制命令分支，验证查询不改变任务结果。"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

class OtaStatusPublicationTest(unittest.TestCase):
    def test_queries_preserve_failure_and_recover_verified_intent(self):
        source = (ROOT/'OtaManager.cpp').read_text()
        start = source.index('    if (command == "CAPS")')
        end = source.index('    if (command.startsWith("PAUSE|"))',start)
        branches = source[start:end]
        prefix = r'''
#include "Arduino.h"
#include <cassert>
#include <iostream>
#define FIRMWARE_PARTITION_PROFILE "test-profile"
struct Partition { int address=0; int size=1024; } part;
using esp_partition_t=Partition;
struct esp_image_metadata_t { uint32_t image_len=1024; };
struct esp_partition_pos_t { int offset; int size; };
struct esp_app_desc_t { const char* version="0.4.0"; };
constexpr int ESP_OK=0, ESP_IMAGE_VERIFY_SILENT=0;
bool failInfo=false;
const Partition* esp_ota_get_running_partition() { return &part; }
int esp_image_verify(int,const esp_partition_pos_t*,esp_image_metadata_t*) { return failInfo ? 1 : 0; }
int esp_ota_get_partition_description(const Partition*,esp_app_desc_t*) { return 0; }
String calculatePartitionDigest(const Partition*,uint32_t) { return "digest"; }
int statisticsStoreSchemaVersion() { return 4; }
String getStableDeviceId() { return "device"; }
struct Checkpoint { String taskId="task",stableDeviceId="device",version="0.4.0",digest="digest",packageDigest="package"; uint32_t offset=1024,packageOffset=512,imageSize=1024,packageSize=512; bool compressedPackage=true,deltaPackage=false,installIntent=true; };
using OtaCheckpoint=Checkpoint;
Checkpoint currentCheckpoint;
struct Store { bool isRejectedImage(const String&) { return false; } } checkpointStore;
bool loadCheckpoint(Checkpoint* cp) { *cp=currentCheckpoint; return true; }
bool active=false,installedTask=false,verifiedForInstall=false,compressedPackage=true,deltaPackage=false;
uint32_t packageReceivedSize=0,receivedSize=0,packageCheckpointOffset=0,checkpointOffset=0,expectedPackageSize=512,expectedSize=1024;
String expectedVersion="0.4.0",expectedDigest="digest",lastStatus,response;
int ownerConnection=7;
namespace OtaBleAdapter {
 void publishStatus(const String& text,int) { response=text; }
 int availableDataSlots() { return 4; }
}
void setStatus(const String& value) { lastStatus=value; response=value; }
struct Message { int connection=7; } message;
void execute(const String& command) {
'''
        suffix = r'''
}
int main() {
 lastStatus="PAUSED|0|512|0.4.0|digest|task|device|package|ERROR:SIGNATURE";
 const auto failure=lastStatus;
 execute("CAPS"); assert(response.startsWith("CAPS|")); assert(lastStatus==failure);
 execute("INFO"); assert(response.startsWith("INFO|")); assert(lastStatus==failure);
 failInfo=true; execute("INFO"); assert(response=="ERROR|RUNNING_IMAGE_INFO"); assert(lastStatus==failure);
 execute("QUERY"); assert(response==failure);
 verifiedForInstall=true; currentCheckpoint.installIntent=true;
 lastStatus="unrelated stale response";
 execute("CAPS"); execute("QUERY");
 assert(response=="VERIFIED|0.4.0|digest|task|device");
 std::cout << "queries preserve task failure and rebuild verified intent\n";
}
'''
        compiler=shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            self.skipTest('需要本机 C++ 编译器')
        with tempfile.TemporaryDirectory() as folder:
            test=Path(folder)/'ota.cpp'; test.write_text(prefix+branches+suffix)
            executable=Path(folder)/'ota'
            build=subprocess.run([compiler,'-std=c++17','-I',str(ROOT/'tests/native_support'),str(test),'-o',str(executable)],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            result=subprocess.run([str(executable)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
