#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// 显式小端序列化，避免结构填充和 core/编译器变化影响持久记录。
namespace WifiIdentityRecord {
constexpr size_t kHeader = 40, kMaxCertificate = 1024, kMaxKey = 256;
constexpr size_t kMaximum = kHeader + kMaxCertificate + kMaxKey;
constexpr uint32_t kMagic = 0x3149564d;  // MVI1
enum class State { kValid, kCorrupt, kUnsupported };
inline uint16_t get16(const uint8_t* p) { return p[0] | uint16_t(p[1]) << 8; }
inline uint32_t get32(const uint8_t* p) { return get16(p) | uint32_t(get16(p + 2)) << 16; }
inline void put16(uint8_t* p, uint16_t value) { p[0] = value; p[1] = value >> 8; }
inline void put32(uint8_t* p, uint32_t value) { put16(p, value); put16(p + 2, value >> 16); }
inline uint32_t checksum(const uint8_t* data, size_t length) {
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < length; ++i) {
    if (i >= 36 && i < 40) continue;
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}
inline State inspect(const uint8_t* data, size_t length) {
  if (!data || length < 8 || get32(data) != kMagic) return State::kCorrupt;
  if (get16(data + 4) != 1) return State::kUnsupported;
  if (length < kHeader || length > kMaximum || get16(data + 6) != kHeader) return State::kCorrupt;
  const size_t cert = get16(data + 32), key = get16(data + 34);
  if (!get32(data + 8) || !get32(data + 12) || !cert || cert > kMaxCertificate ||
      !key || key > kMaxKey || length != kHeader + cert + key ||
      get32(data + 36) != checksum(data, length)) return State::kCorrupt;
  uint8_t nonzero = 0;
  for (size_t i = 16; i < 32; ++i) nonzero |= data[i];
  return nonzero ? State::kValid : State::kCorrupt;
}
inline bool seal(uint8_t* record, size_t length, uint32_t sequence, uint32_t generation,
                 size_t certLength, size_t keyLength) {
  if (!record || certLength > kMaxCertificate || keyLength > kMaxKey ||
      length != kHeader + certLength + keyLength) return false;
  put32(record, kMagic); put16(record + 4, 1); put16(record + 6, kHeader);
  put32(record + 8, sequence); put32(record + 12, generation);
  put16(record + 32, certLength); put16(record + 34, keyLength);
  put32(record + 36, checksum(record, length));
  return inspect(record, length) == State::kValid;
}
// 任一未知版本都停止加载，避免旧固件静默恢复过时信任身份。
inline int select(const uint8_t* a, size_t aSize, const uint8_t* b, size_t bSize) {
  const State as = inspect(a, aSize), bs = inspect(b, bSize);
  if (as == State::kUnsupported || bs == State::kUnsupported) return -1;
  if (as != State::kValid) return bs == State::kValid ? 1 : -1;
  if (bs != State::kValid) return 0;
  // 本格式禁止序号回绕；达到 UINT32_MAX 必须显式维护。
  return get32(b + 8) > get32(a + 8) ? 1 : 0;
}
}  // namespace WifiIdentityRecord
