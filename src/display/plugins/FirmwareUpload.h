// Installs a display firmware image received in chunks. The image goes to the inactive OTA partition and only becomes
// the boot image after the whole file has passed FirmwareImageCheck and esp_ota_end's checksum verification, so a bad
// or interrupted upload leaves the running firmware in place.
#pragma once

#include <cstddef>
#include <cstdint>
#include <display/util/FirmwareImageCheck.h>

class FirmwareUpload {
  public:
    // Each returns false and sets error() on failure; after a failure the partition has been released.
    bool begin(size_t total, const char *requiredMarker);
    bool write(const uint8_t *data, size_t len);
    bool finish();
    void abort();

    bool active() const { return open; }
    const char *error() const { return lastError; }

  private:
    void fail(const char *message);
    void closeFlash();
    void openWatchdogWindow();
    void closeWatchdogWindow();

    FirmwareImageCheck check;
    const char *lastError = "";
    bool open = false;
    uint32_t flashHandle = 0;
    const void *partition = nullptr;
};
