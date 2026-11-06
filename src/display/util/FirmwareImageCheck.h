// Streaming sanity check for an uploaded firmware image. esp_ota_end proves an image is intact, but not that it is
// meant for the board it is about to be installed on: the display and controller builds are both valid ESP32-S3
// images, and both are named firmware.bin.
//
// Every display build embeds its own release asset name (OTA_DISPLAY_FIRMWARE), which the controller, bootloader and
// other display variant do not, so that string identifies the right build across firmware versions. A display upload
// requires it; a controller upload forbids it, which is the mistake worth catching — a display image sent to the
// controller over BLE would brick the board that cannot be reflashed without opening the machine.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

class FirmwareImageCheck {
  public:
    enum class MarkerPolicy {
        REQUIRED, // the image must carry the marker (installing display firmware on the display)
        FORBIDDEN // the image must not carry it (installing controller firmware, which never does)
    };

    void begin(size_t total, size_t maxSize, const char *requiredMarker, MarkerPolicy markerPolicy = MarkerPolicy::REQUIRED) {
        expected = total;
        received = 0;
        marker = requiredMarker;
        markerLen = std::strlen(requiredMarker);
        matched = 0;
        markerFound = false;
        policy = markerPolicy;
        problem = nullptr;
        if (markerLen == 0 || markerLen > MAX_MARKER) {
            problem = "Internal error: invalid firmware marker";
            return;
        }
        buildFailureTable();
        if (total == 0) {
            problem = "The file is empty";
        } else if (total > maxSize) {
            problem = policy == MarkerPolicy::REQUIRED ? "The file is too large to be display firmware"
                                                       : "The file is too large to be controller firmware";
        }
    }

    void feed(const uint8_t *data, size_t len) {
        if (problem != nullptr) {
            return;
        }
        for (size_t i = 0; i < len; i++) {
            const size_t offset = received + i;
            if (offset == OFFSET_MAGIC && data[i] != IMAGE_MAGIC) {
                problem = "This is not a firmware image";
                return;
            }
            if (offset == OFFSET_CHIP_ID && data[i] != CHIP_ID_ESP32S3) {
                problem = "This firmware is built for a different chip";
                return;
            }
            if (offset == OFFSET_CHIP_ID + 1 && data[i] != 0) {
                problem = "This firmware is built for a different chip";
                return;
            }
            scanMarker(data[i]);
            if (markerFound && policy == MarkerPolicy::FORBIDDEN) {
                // Known as soon as it is seen, so the upload stops instead of draining a whole display image.
                problem = "This is the display firmware file. Install the controller file (board-firmware.bin) here.";
                return;
            }
        }
        received += len;
        if (received > expected) {
            problem = "The upload is larger than announced";
        }
    }

    // Call after the last chunk: the marker can only be known to be missing at the end.
    bool complete() {
        if (problem == nullptr && received != expected) {
            problem = "The upload ended early";
        }
        if (problem == nullptr && received <= OFFSET_CHIP_ID + 1) {
            problem = "This is not a firmware image";
        }
        if (problem == nullptr && policy == MarkerPolicy::REQUIRED && !markerFound) {
            problem = "This firmware is not for this machine's display board. Use the display firmware file.";
        }
        return problem == nullptr;
    }

    bool failed() const { return problem != nullptr; }
    const char *error() const { return problem != nullptr ? problem : ""; }

  private:
    static constexpr size_t MAX_MARKER = 48;
    static constexpr size_t OFFSET_MAGIC = 0;
    static constexpr size_t OFFSET_CHIP_ID = 12; // esp_image_header_t.chip_id (little-endian uint16)
    static constexpr uint8_t IMAGE_MAGIC = 0xE9;
    static constexpr uint8_t CHIP_ID_ESP32S3 = 9;

    // Knuth-Morris-Pratt, so a marker split across two network chunks is still found.
    void buildFailureTable() {
        failure[0] = 0;
        size_t k = 0;
        for (size_t i = 1; i < markerLen; i++) {
            while (k > 0 && marker[i] != marker[k]) {
                k = failure[k - 1];
            }
            if (marker[i] == marker[k]) {
                k++;
            }
            failure[i] = static_cast<uint8_t>(k);
        }
    }

    void scanMarker(uint8_t byte) {
        if (markerFound) {
            return;
        }
        while (matched > 0 && static_cast<uint8_t>(marker[matched]) != byte) {
            matched = failure[matched - 1];
        }
        if (static_cast<uint8_t>(marker[matched]) == byte) {
            matched++;
        }
        if (matched == markerLen) {
            markerFound = true;
        }
    }

    size_t expected = 0;
    size_t received = 0;
    const char *marker = "";
    size_t markerLen = 0;
    size_t matched = 0;
    bool markerFound = false;
    MarkerPolicy policy = MarkerPolicy::REQUIRED;
    const char *problem = nullptr;
    uint8_t failure[MAX_MARKER] = {};
};
