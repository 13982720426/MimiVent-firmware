// 满载缩容时保留最新记录，并对丢弃的头部推进详情边界。
#define main legacyMigrationMain
#include "statistics_migration_harness.cpp"
#undef main

int main() {
  StoreHeader source{};
  source.magic = kMagic;
  source.schema = kSchemaV6;
  source.eventCount = 512;
  source.nextEventId = 513;
  std::vector<EventRecord> sourceEvents(512);
  for (size_t i = 0; i < sourceEvents.size(); ++i) {
    sourceEvents[i].id = i + 1;
    sourceEvents[i].unixSeconds = 1793203200 + i;
  }
  Crc32 crc;
  crc.update(reinterpret_cast<uint8_t*>(&source), sizeof(source));
  crc.update(reinterpret_cast<uint8_t*>(sourceEvents.data()), sourceEvents.size() * sizeof(EventRecord));
  source.checksum = crc.finish();
  auto& bytes = files["/resize.bin"];
  bytes.resize(sizeof(source) + sourceEvents.size() * sizeof(EventRecord));
  memcpy(bytes.data(), &source, sizeof(source));
  memcpy(bytes.data() + sizeof(source), sourceEvents.data(), sourceEvents.size() * sizeof(EventRecord));
  StoreHeader result{};
  assert(readSlotV5("/resize.bin", &result, true));
  assert(events[0].id == 257);
  assert(events[255].id == 512);
  assert(result.droppedThrough == sourceEvents[255].unixSeconds);
  // 已丢弃事件的字节也必须参与 CRC。
  bytes[sizeof(source)] ^= 1;
  assert(!readSlotV5("/resize.bin", &result, true));
}
