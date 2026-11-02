#pragma once

#include <stddef.h>
#include <stdint.h>

// OTA 分块采用独立 raw-deflate 流，断点恢复时无需恢复压缩器内部状态。
bool otaInflateBlock(const uint8_t* compressed, size_t compressedLength,
                     uint8_t* output, size_t outputCapacity,
                     size_t* outputLength);

// 实验动态工作区模式按需申请解压状态，暂停/失败/校验时由 owner 释放。
bool otaCompressionPrepare();
void otaCompressionRelease();
