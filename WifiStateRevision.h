#pragma once

#include <stdint.h>

// 在单次启动内为业务状态分配 32 位序号；MQTT 心跳不推进序号。
class WifiStateRevision {
 public:
  explicit WifiStateRevision(uint32_t initialSequence = 0)
      : sequence_(initialSequence) {}

  bool observe(uint32_t fingerprint) {
    if (!initialized_) {
      initialized_ = true;
      fingerprint_ = fingerprint;
      return false;
    }
    if (fingerprint_ == fingerprint) return false;
    fingerprint_ = fingerprint;
    ++sequence_;
    return true;
  }

  uint32_t sequence() const { return sequence_; }

  // RFC 1982：半范围及更远的差值存在歧义，不视作更新。
  static bool isNewer(uint32_t incoming, uint32_t current) {
    const uint32_t distance = incoming - current;
    return distance != 0 && distance < 0x80000000u;
  }

 private:
  uint32_t sequence_;
  uint32_t fingerprint_ = 0;
  bool initialized_ = false;
};
