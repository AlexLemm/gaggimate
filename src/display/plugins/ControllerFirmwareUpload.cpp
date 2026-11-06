#include "ControllerFirmwareUpload.h"

#include <ControllerOTA.h>
#include <LittleFS.h>
#include <esp_log.h>

namespace {
constexpr const char *TAG = "ControllerFirmwareUpload";
// The image is written here and renamed onto CONTROLLER_FIRMWARE_PATH only once the whole upload has validated, so an
// interrupted upload can never be mistaken for a complete one by ControllerOTA::updateStaged.
constexpr const char *STAGING_PATH = "/board-firmware.part";
// ota_0 in the controller's default_8MB.csv partition table; see the controller build's reported flash size.
constexpr size_t CONTROLLER_APP_PARTITION = 3342336;
// LittleFS also holds profiles and shot history. Keep a margin free so staging a firmware image cannot fill the
// filesystem the machine needs for its own data.
constexpr size_t FS_FREE_MARGIN = 64 * 1024;
} // namespace

bool ControllerFirmwareUpload::begin(size_t total, const char *displayMarker) {
    lastError = "";
    discard();

    check.begin(total, CONTROLLER_APP_PARTITION, displayMarker, FirmwareImageCheck::MarkerPolicy::FORBIDDEN);
    if (check.failed()) {
        lastError = check.error();
        return false;
    }

    const size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
    if (total + FS_FREE_MARGIN > freeBytes) {
        // Shot history is the usual culprit, and it is the one thing here the user can clear.
        lastError = "Not enough space left on the machine to stage the firmware. Export and clear the shot history, "
                    "then try again.";
        return false;
    }

    file = LittleFS.open(STAGING_PATH, FILE_WRITE, true);
    if (!file) {
        lastError = "Could not open a staging file on the machine";
        return false;
    }
    open = true;
    return true;
}

bool ControllerFirmwareUpload::write(const uint8_t *data, size_t len) {
    if (!open) {
        return false;
    }
    check.feed(data, len);
    if (check.failed()) {
        fail(check.error());
        return false;
    }
    if (file.write(data, len) != len) {
        fail("Writing the firmware to the machine failed");
        return false;
    }
    return true;
}

bool ControllerFirmwareUpload::finish() {
    if (!open) {
        return false;
    }
    if (!check.complete()) {
        fail(check.error());
        return false;
    }
    file.close();
    open = false;

    if (LittleFS.exists(CONTROLLER_FIRMWARE_PATH)) {
        LittleFS.remove(CONTROLLER_FIRMWARE_PATH);
    }
    if (!LittleFS.rename(STAGING_PATH, CONTROLLER_FIRMWARE_PATH)) {
        LittleFS.remove(STAGING_PATH);
        lastError = "Could not store the firmware on the machine";
        return false;
    }
    ESP_LOGI(TAG, "Controller firmware staged at %s", CONTROLLER_FIRMWARE_PATH);
    return true;
}

void ControllerFirmwareUpload::abort() {
    if (open) {
        ESP_LOGW(TAG, "Controller firmware upload aborted");
    }
    discard();
}

void ControllerFirmwareUpload::fail(const char *message) {
    ESP_LOGE(TAG, "%s", message);
    discard();
    lastError = message;
}

void ControllerFirmwareUpload::discard() {
    if (file) {
        file.close();
    }
    open = false;
    if (LittleFS.exists(STAGING_PATH)) {
        LittleFS.remove(STAGING_PATH);
    }
}
