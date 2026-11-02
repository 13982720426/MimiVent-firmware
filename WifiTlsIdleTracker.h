#pragma once
#include <stdint.h>

// 调用方在同一临界区保护；只保存 POD，不持有 socket 或分配内存。
class WifiTlsIdleTracker {
 public:
  struct Work { int socket; uint32_t generation; };
  void opened(int socket, uint32_t now) {
    socket_ = socket; ++generation_; lastActivity_ = now;
  }
  void closed(int socket) {
    if (socket == socket_) { socket_ = -1; ++generation_; }
  }
  void touch(int socket, uint32_t now) {
    if (socket == socket_) lastActivity_ = now;
  }
  bool schedule(uint32_t now, uint32_t timeout, Work* work) {
    if (pending_ || socket_ < 0 || now - lastActivity_ < timeout) return false;
    pending_ = true; *work = {socket_, generation_}; return true;
  }
  bool valid(const Work& work, uint32_t now, uint32_t timeout) const {
    return work.socket == socket_ && work.generation == generation_ &&
        socket_ >= 0 && now - lastActivity_ >= timeout;
  }
  void completed() { pending_ = false; }
 private:
  int socket_ = -1;
  uint32_t generation_ = 0, lastActivity_ = 0;
  bool pending_ = false;
};
