// Stages a controller firmware image received in chunks into LittleFS, where ControllerOTA::updateStaged picks it up
// and streams it to the controller over BLE. The display's own OTA partition is not involved: this image is not for
// this board.
//
// Unlike a display upload, a bad image here cannot be undone from the web UI — the controller has no second OTA slot
// the display can fall back to, and recovering it means a USB cable and an open machine. So the file is validated
// while it arrives and only renamed into place once the whole upload has passed.
#pragma once

#include <FS.h>
#include <cstddef>
#include <cstdint>
#include <display/util/FirmwareImageCheck.h>

class ControllerFirmwareUpload {
  public:
    // Each returns false and sets error() on failure; after a failure the partial file has been removed.
    bool begin(size_t total, const char *displayMarker);
    bool write(const uint8_t *data, size_t len);
    bool finish();
    void abort();

    bool active() const { return open; }
    const char *error() const { return lastError; }

  private:
    void fail(const char *message);
    void discard();

    FirmwareImageCheck check;
    File file;
    const char *lastError = "";
    bool open = false;
};
