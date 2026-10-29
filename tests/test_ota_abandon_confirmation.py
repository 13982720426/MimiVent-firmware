"""验证 ABANDON 确认发送到发起命令的 BLE 连接。"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class OtaAbandonConfirmationTest(unittest.TestCase):
    def test_abandon_response_targets_request_connection(self):
        source = (ROOT / "OtaManager.cpp").read_text()
        start = source.index('    if (command.startsWith("ABANDON|"))')
        end = source.index('    if (command.startsWith("INSTALL|"))', start)
        branch = source[start:end]
        prefix = r'''
#include "Arduino.h"
#include <cassert>
#include <iostream>
struct Checkpoint {
 String taskId="task"; bool installIntent=false; uint32_t partitionAddress=0x20000;
};
using OtaCheckpoint=Checkpoint;
Checkpoint saved;
bool active=false, installedTask=false, verifiedForInstall=false;
int ownerConnection=0, publishedConnection=-1, messageConnection=42;
struct Partition { uint32_t address; } bootPartition{0x10000};
using esp_partition_t=Partition;
const esp_partition_t* esp_ota_get_boot_partition() { return &bootPartition; }
void* targetPartition=nullptr;
String lastStatus="PAUSED|0|100|version|digest|task|device", response;
struct Store {
 bool isRejectedTask(const char*) { return false; }
 bool clearRejectedImage(const char*) { return true; }
} checkpointStore;
struct Message { int connection=messageConnection; } message;
namespace OtaBleAdapter {
 void publishStatus(const String& value, int connection) {
   response=value; publishedConnection=connection;
 }
}
String commandTaskId(const String& command) {
 const int separator=command.indexOf('|');
 const int requestSeparator=command.indexOf('|',separator+1);
 return command.substring(separator+1,requestSeparator<0?command.length():requestSeparator);
}
bool loadCheckpoint(Checkpoint* value) { *value=saved; return true; }
bool checkpointCleared=false;
bool clearCheckpoint() { checkpointCleared=true; return true; }
bool installCandidateWasRejected() { return false; }
void setStatus(const String& value) {
 lastStatus=value; response=value; publishedConnection=ownerConnection;
}
void execute(const String& command) {
'''
        suffix = r'''
}
int main() {
 execute("ABANDON|task|9");
 assert(response=="IDLE");
 assert(publishedConnection==message.connection);
 assert(ownerConnection==message.connection);
 assert(checkpointCleared);
 lastStatus="PAUSED|state"; active=true; response=""; publishedConnection=-1;
 execute("ABANDON|task|10");
 assert(response=="ERROR|BUSY");
 assert(publishedConnection==message.connection);
 assert(lastStatus=="PAUSED|state");
 active=false; saved.installIntent=true; checkpointCleared=false; response="";
 bootPartition.address=0x10000;
 execute("ABANDON|task|11");
 assert(response=="IDLE");
 assert(checkpointCleared);
 saved.installIntent=true; checkpointCleared=false; response="";
 bootPartition.address=saved.partitionAddress;
 execute("ABANDON|task|12");
 assert(response=="ERROR|INSTALL_COMMITTED");
 assert(!checkpointCleared);
 std::cout << "ABANDON response uses the requesting BLE connection\n";
}
'''
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("需要本机 C++ 编译器")
        with tempfile.TemporaryDirectory() as folder:
            test = Path(folder) / "ota.cpp"
            test.write_text(prefix + branch + suffix)
            executable = Path(folder) / "ota"
            build = subprocess.run(
                [compiler, "-std=c++17", "-I", str(ROOT / "tests/native_support"), str(test), "-o", str(executable)],
                capture_output=True, text=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
