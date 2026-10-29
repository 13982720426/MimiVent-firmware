#include <esp_app_desc.h>
#include <esp_idf_version.h>

#include "FirmwareVersion.h"

#define MIMIVENT_STRINGIFY_INNER(value) #value
#define MIMIVENT_STRINGIFY(value) MIMIVENT_STRINGIFY_INNER(value)

// Arduino-ESP32 预编译库默认把库构建哈希写入 app_desc.version；OTA 需要稳定的语义版本。
extern "C" {
extern const esp_app_desc_t esp_app_desc
    __attribute__((section(".flash.appdesc"), used)) = {
        ESP_APP_DESC_MAGIC_WORD,
        0,
        {0, 0},
        FIRMWARE_VERSION,
        "MimiVent",
        __TIME__,
        __DATE__,
        "ESP-IDF " MIMIVENT_STRINGIFY(ESP_IDF_VERSION_MAJOR) "."
            MIMIVENT_STRINGIFY(ESP_IDF_VERSION_MINOR) "."
            MIMIVENT_STRINGIFY(ESP_IDF_VERSION_PATCH),
        {0},
        0,
        0,
        0,
        {0, 0, 0},
        {0},
};
}
