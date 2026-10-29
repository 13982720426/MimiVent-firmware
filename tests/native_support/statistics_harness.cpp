#include <ctime>
#include <sys/time.h>
#include <cassert>
#include "Arduino.h"
inline time_t fakeNow=1793287800; // UTC+8 的 2026-10-29 23:30。
inline time_t testTime(time_t* out) { if(out) *out=fakeNow; return fakeNow; }
inline int testSetTime(const timeval* value, const void*) { fakeNow=value->tv_sec; return 0; }
#define time testTime
#define settimeofday testSetTime
#include "../../StatisticsStore.cpp"
#undef time
#undef settimeofday
void emit(const char* name,const String& json) { std::cout << name << '\t' << json.c_str() << '\n'; }
int main() {
  statisticsStoreBegin();
  assert(statisticsStoreSchemaVersion()==4);
  assert(statisticsStoreSetClock(fakeNow,480));
  statisticsStoreFanStarted(kForce,0);
  for(int i=0;i<600;i++) statisticsStoreRecordActivity();
  assert(eventCount==512);
  assert(activeRun.id!=0); // 明细可淘汰，但运行结算状态不能丢。
  fakeNow+=7200;
  emit("active_today",statisticsStoreBuildJson("today",true,7200000,0));
  emit("active_threeDays",statisticsStoreBuildJson("threeDays",true,7200000,0));
  statisticsStoreFanStopped(7200000);
  statisticsStoreFanStopped(7200000); // 重复停止不得重复入账。
  emit("today",statisticsStoreBuildJson("today",false,0,0));
  emit("yesterday",statisticsStoreBuildJson("yesterday",false,0,0));
  emit("threeDays",statisticsStoreBuildJson("threeDays",false,0,0));
  emit("sevenDays",statisticsStoreBuildJson("sevenDays",false,0,0));
  emit("events",statisticsStoreBuildEventsJson(fakeNow-86400,fakeNow+1,"all",0,32,0,0));
  memset(daily,0,sizeof(daily)); memset(events,0,sizeof(events));
  dailyCount=eventCount=generation=0; droppedThroughEpoch=0; ready=false;
  statisticsStoreBegin();
  emit("restored",statisticsStoreBuildJson("threeDays",false,0,0));
  // 旧 schema 数据无法按新结构加载，必须切换数据批次。
  const std::string previousEpoch=statisticsStoreDataEpoch();
  for(auto& entry: files) if(entry.second.size()>=sizeof(StoreHeader)) {
    uint16_t schema=3; memcpy(entry.second.data()+offsetof(StoreHeader,schema),&schema,2);
  }
  statisticsStoreBegin();
  assert(previousEpoch!=statisticsStoreDataEpoch());
  emit("reset",statisticsStoreBuildJson("today",false,0,0));
  // 无时钟时发生的运行不能被 PIR 洪流挤掉，首次校时仍应归账。
  fakeNow=0; fakeMillis=10000;
  statisticsStoreFanStarted(kForce,fakeMillis);
  for(int i=0;i<600;i++) statisticsStoreRecordActivity();
  fakeMillis=70000;
  statisticsStoreFanStopped(60000);
  assert(statisticsStoreSetClock(1793298600,480));
  emit("unsynced",statisticsStoreBuildJson("today",false,0,0));
}
