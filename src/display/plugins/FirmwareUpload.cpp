#include "FirmwareUpload.h"

#include <esp_log.h>

#ifndef GAGGIMATE_SIM
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#endif

namespace {
constexpr const char *TAG = "FirmwareUpload";
#ifdef GAGGIMATE_SIM
constexpr size_t SIM_PARTITION_SIZE = 8 * 1024 * 1024;
#else
// Same allowance GitHubOTA gives its updates: flash writes hold up the tasks the watchdog monitors.
constexpr int UPLOAD_WDT_TIMEOUT_S = 60;
#endif
} // namespace

void FirmwareUpload::openWatchdogWindow() {
#ifndef GAGGIMATE_SIM
    esp_task_wdt_init(UPLOAD_WDT_TIMEOUT_S, true);
#endif
}

void FirmwareUpload::closeWatchdogWindow() {
#ifndef GAGGIMATE_SIM
    esp_task_wdt_init(CONFIG_ESP_TASK_WDT_TIMEOUT_S, true);
#endif
}

void FirmwareUpload::closeFlash() {
#ifndef GAGGIMATE_SIM
    if (flashHandle != 0) {
        esp_ota_abort(flashHandle);
    }
#endif
    flashHandle = 0;
}

void FirmwareUpload::fail(const char *message) {
    ESP_LOGE(TAG, "%s", message);
    lastError = message;
    if (open) {
        closeFlash();
        closeWatchdogWindow();
        open = false;
    }
}

void FirmwareUpload::abort() {
    if (open) {
        fail("Upload cancelled");
    }
}

bool FirmwareUpload::begin(size_t total, const char *requiredMarker) {
    lastError = "";
#ifdef GAGGIMATE_SIM
    check.begin(total, SIM_PARTITION_SIZE, requiredMarker);
#else
    const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
    if (target == nullptr) {
        fail("This machine has no partition available for updates");
        return false;
    }
    partition = target;
    check.begin(total, target->size, requiredMarker);
#endif
    if (check.failed()) {
        fail(check.error());
        return false;
    }
#ifndef GAGGIMATE_SIM
    openWatchdogWindow();
    // Sequential writes erase one sector at a time. Erasing the whole image up front would block the web server's task
    // for seconds, because this runs inside its upload callback.
    esp_ota_handle_t handle = 0;
    if (esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) {
        closeWatchdogWindow();
        fail("Could not prepare the update partition");
        return false;
    }
    flashHandle = handle;
#endif
    open = true;
    return true;
}

bool FirmwareUpload::write(const uint8_t *data, size_t len) {
    if (!open) {
        return false;
    }
    check.feed(data, len);
    if (check.failed()) {
        fail(check.error());
        return false;
    }
#ifndef GAGGIMATE_SIM
    if (esp_ota_write(flashHandle, data, len) != ESP_OK) {
        fail("Writing to flash failed");
        return false;
    }
#endif
    return true;
}

bool FirmwareUpload::finish() {
    if (!open) {
        return false;
    }
    if (!check.complete()) {
        fail(check.error());
        return false;
    }
#ifdef GAGGIMATE_SIM
    ESP_LOGI(TAG, "Simulator: image accepted, nothing flashed");
#else
    // esp_ota_end verifies the complete image (header, checksum, appended SHA-256) before it can be booted.
    const esp_err_t endResult = esp_ota_end(flashHandle);
    flashHandle = 0; // esp_ota_end releases the handle whatever its result
    if (endResult != ESP_OK) {
        fail("The file is damaged or not a complete firmware image");
        return false;
    }
    if (esp_ota_set_boot_partition(static_cast<const esp_partition_t *>(partition)) != ESP_OK) {
        fail("Could not select the new firmware for startup");
        return false;
    }
#endif
    closeWatchdogWindow();
    open = false;
    return true;
}
