#pragma once
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>

// 只由 OTA owner loop 使用；输入与输出独立分配，避免 TLS 后的堆碎片要求连续 32 KiB。
class OtaTransferWorkspace {
 public:
  using Allocate = void* (*)(size_t);
  using Release = void (*)(void*);
  explicit OtaTransferWorkspace(Allocate allocate = malloc, Release release = free)
      : allocate_(allocate), release_(release) {}
  ~OtaTransferWorkspace() { reset(); }
  OtaTransferWorkspace(const OtaTransferWorkspace&) = delete;
  OtaTransferWorkspace& operator=(const OtaTransferWorkspace&) = delete;
  bool reserve(size_t blockBytes) {
    if (blockBytes == 0 || blockBytes > SIZE_MAX / 2) return false;
    if (memory_ && output_ && blockBytes_ == blockBytes) return true;
    reset();
    memory_ = static_cast<uint8_t*>(allocate_(blockBytes));
    if (!memory_) return false;
    output_ = static_cast<uint8_t*>(allocate_(blockBytes));
    if (!output_) { reset(); return false; }
    blockBytes_ = blockBytes;
    return true;
  }
  void reset() {
    if (memory_) release_(memory_);
    if (output_) release_(output_);
    memory_ = nullptr;
    output_ = nullptr;
    blockBytes_ = 0;
  }
  uint8_t* input() const { return memory_; }
  uint8_t* output() const { return output_; }
  size_t capacity() const { return blockBytes_; }
 private:
  Allocate allocate_;
  Release release_;
  uint8_t* memory_ = nullptr;
  uint8_t* output_ = nullptr;
  size_t blockBytes_ = 0;
};
