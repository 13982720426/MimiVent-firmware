#include "OtaCompression.h"

#include <climits>
#include <esp_timer.h>

#include "miniz.h"

bool otaInflateBlock(const uint8_t* compressed, size_t compressedLength,
                     uint8_t* output, size_t outputCapacity,
                     size_t* outputLength) {
  if (!compressed || !compressedLength || !output || !outputCapacity ||
      !outputLength || compressedLength > UINT_MAX ||
      outputCapacity > UINT_MAX) {
    return false;
  }

  // otaInflateBlock 仅由 loop 任务串行调用，避免 8 KiB 解压状态占用任务栈。
  static tinfl_decompressor decompressor;
  tinfl_init(&decompressor);
  size_t inputBytes = compressedLength;
  size_t outputBytes = outputCapacity;
  const int64_t startedAtUs = esp_timer_get_time();
  const tinfl_status status = tinfl_decompress(
      &decompressor, compressed, &inputBytes, output, output,
      &outputBytes, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  const int64_t elapsedUs = esp_timer_get_time() - startedAtUs;
  if (elapsedUs > 100000 || status != TINFL_STATUS_DONE ||
      inputBytes != compressedLength ||
      outputBytes > outputCapacity) {
    return false;
  }
  *outputLength = outputBytes;
  return true;
}
