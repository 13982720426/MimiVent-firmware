#include "OtaCompression.h"

#include <climits>
#include <cstdlib>
#include <esp_timer.h>

#include "miniz.h"
#include <cstdio>

namespace {
#if defined(MIMIVENT_OTA_DYNAMIC_WORKSPACE) && MIMIVENT_OTA_DYNAMIC_WORKSPACE
tinfl_decompressor* decompressor = nullptr;
#else
tinfl_decompressor fixedDecompressor;
tinfl_decompressor* decompressor = &fixedDecompressor;
#endif
}

bool otaCompressionPrepare() {
#if defined(MIMIVENT_OTA_DYNAMIC_WORKSPACE) && MIMIVENT_OTA_DYNAMIC_WORKSPACE
  if (!decompressor) decompressor = static_cast<tinfl_decompressor*>(malloc(sizeof(tinfl_decompressor)));
#endif
  return decompressor != nullptr;
}

void otaCompressionRelease() {
#if defined(MIMIVENT_OTA_DYNAMIC_WORKSPACE) && MIMIVENT_OTA_DYNAMIC_WORKSPACE
  free(decompressor);
  decompressor = nullptr;
#endif
}

bool otaInflateBlock(const uint8_t* compressed, size_t compressedLength,
                     uint8_t* output, size_t outputCapacity,
                     size_t* outputLength) {
  if (!compressed || !compressedLength || !output || !outputCapacity ||
      !outputLength || compressedLength > UINT_MAX ||
      outputCapacity > UINT_MAX) {
    return false;
  }

  // otaInflateBlock 仅由 loop 任务串行调用，避免 8 KiB 解压状态占用任务栈。
  if (!otaCompressionPrepare()) return false;
  tinfl_init(decompressor);
  size_t inputBytes = compressedLength;
  size_t outputBytes = outputCapacity;
  const int64_t startedAtUs = esp_timer_get_time();
  const tinfl_status status = tinfl_decompress(
      decompressor, compressed, &inputBytes, output, output,
      &outputBytes, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  const int64_t elapsedUs = esp_timer_get_time() - startedAtUs;
  if (elapsedUs > 100000 || status != TINFL_STATUS_DONE ||
      inputBytes != compressedLength ||
      outputBytes > outputCapacity) {
    printf("[ota-inflate] rejected status=%d input=%u/%u output=%u elapsedUs=%lld\n",status,
        unsigned(inputBytes),unsigned(compressedLength),unsigned(outputBytes),(long long)elapsedUs);
    return false;
  }
  *outputLength = outputBytes;
  return true;
}
