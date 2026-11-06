#include <algorithm>
#include <display/util/FirmwareImageCheck.h>
#include <string>
#include <unity.h>
#include <vector>

void setUp() {}
void tearDown() {}

static constexpr size_t MAX_SIZE = 1024;
static constexpr const char *MARKER = "display-firmware.bin";

static std::vector<uint8_t> image(const std::string &body, uint8_t magic = 0xE9, uint8_t chip = 9) {
    std::vector<uint8_t> bytes(16, 0);
    bytes[0] = magic;
    bytes[12] = chip;
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}

static bool checkInChunks(const std::vector<uint8_t> &bytes, size_t chunk) {
    FirmwareImageCheck check;
    check.begin(bytes.size(), MAX_SIZE, MARKER);
    for (size_t i = 0; i < bytes.size(); i += chunk) {
        check.feed(bytes.data() + i, std::min(chunk, bytes.size() - i));
    }
    return check.complete();
}

// A controller upload runs the same scan inverted: the display marker must be absent.
static bool checkAsControllerImage(const std::vector<uint8_t> &bytes, size_t chunk) {
    FirmwareImageCheck check;
    check.begin(bytes.size(), MAX_SIZE, MARKER, FirmwareImageCheck::MarkerPolicy::FORBIDDEN);
    for (size_t i = 0; i < bytes.size(); i += chunk) {
        check.feed(bytes.data() + i, std::min(chunk, bytes.size() - i));
    }
    return check.complete();
}

void test_accepts_display_image() { TEST_ASSERT_TRUE(checkInChunks(image("xx display-firmware.bin yy"), 4096)); }

void test_controller_upload_accepts_an_image_without_the_display_marker() {
    TEST_ASSERT_TRUE(checkAsControllerImage(image("GaggiMateController"), 4096));
}

// The mistake that matters: a display image streamed onto the controller would brick the board that cannot be
// reflashed without opening the machine.
void test_controller_upload_rejects_the_display_image() {
    TEST_ASSERT_FALSE(checkAsControllerImage(image("xx display-firmware.bin yy"), 4096));
    for (size_t chunk = 1; chunk < 32; chunk++) {
        TEST_ASSERT_FALSE_MESSAGE(checkAsControllerImage(image("xx display-firmware.bin yy"), chunk),
                                  "display marker missed across a chunk boundary");
    }
}

// Rejected as soon as the marker is seen, rather than after draining a whole 3.8 MB image.
void test_controller_upload_fails_before_the_upload_ends() {
    const auto bytes = image("display-firmware.bin and a great deal more content after it");
    FirmwareImageCheck check;
    check.begin(bytes.size(), MAX_SIZE, MARKER, FirmwareImageCheck::MarkerPolicy::FORBIDDEN);
    check.feed(bytes.data(), 40); // enough to cover the header and the marker, not the rest
    TEST_ASSERT_TRUE(check.failed());
}

void test_controller_upload_still_checks_format_and_size() {
    TEST_ASSERT_FALSE(checkAsControllerImage(image("controller", 0x00), 4096)); // not a firmware image
    TEST_ASSERT_FALSE(checkAsControllerImage(image("controller", 0xE9, 5), 4096));
    FirmwareImageCheck check;
    check.begin(MAX_SIZE + 1, MAX_SIZE, MARKER, FirmwareImageCheck::MarkerPolicy::FORBIDDEN);
    TEST_ASSERT_TRUE(check.failed());
}

void test_accepts_whatever_the_chunk_boundaries() {
    const auto bytes = image("xx display-firmware.bin yy");
    for (size_t chunk = 1; chunk < bytes.size(); chunk++) {
        TEST_ASSERT_TRUE_MESSAGE(checkInChunks(bytes, chunk), "marker lost across a chunk boundary");
    }
}

void test_finds_marker_after_a_false_start() {
    TEST_ASSERT_TRUE(checkInChunks(image("display-display-firmware.bin"), 3));
}

void test_rejects_other_builds() {
    TEST_ASSERT_FALSE(checkInChunks(image("display-headless-firmware.bin"), 4096)); // headless variant
    TEST_ASSERT_FALSE(checkInChunks(image("GaggiMateController"), 4096));           // controller or bootloader
}

void test_rejects_wrong_format_or_chip() {
    TEST_ASSERT_FALSE(checkInChunks(image("display-firmware.bin", 0x00), 4096));
    TEST_ASSERT_FALSE(checkInChunks(image("display-firmware.bin", 0xE9, 5), 4096));
}

void test_rejects_bad_sizes() {
    FirmwareImageCheck tooLarge;
    tooLarge.begin(MAX_SIZE + 1, MAX_SIZE, MARKER);
    TEST_ASSERT_TRUE(tooLarge.failed());

    FirmwareImageCheck empty;
    empty.begin(0, MAX_SIZE, MARKER);
    TEST_ASSERT_TRUE(empty.failed());
}

void test_rejects_short_upload() {
    const auto bytes = image("display-firmware.bin");
    FirmwareImageCheck check;
    check.begin(bytes.size() + 1, MAX_SIZE, MARKER);
    check.feed(bytes.data(), bytes.size());
    TEST_ASSERT_FALSE(check.complete());
}

void test_rejects_file_shorter_than_header() {
    const std::vector<uint8_t> bytes = {0xE9, 1, 2};
    TEST_ASSERT_FALSE(checkInChunks(bytes, 4096));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_accepts_display_image);
    RUN_TEST(test_controller_upload_accepts_an_image_without_the_display_marker);
    RUN_TEST(test_controller_upload_rejects_the_display_image);
    RUN_TEST(test_controller_upload_fails_before_the_upload_ends);
    RUN_TEST(test_controller_upload_still_checks_format_and_size);
    RUN_TEST(test_accepts_whatever_the_chunk_boundaries);
    RUN_TEST(test_finds_marker_after_a_false_start);
    RUN_TEST(test_rejects_other_builds);
    RUN_TEST(test_rejects_wrong_format_or_chip);
    RUN_TEST(test_rejects_bad_sizes);
    RUN_TEST(test_rejects_short_upload);
    RUN_TEST(test_rejects_file_shorter_than_header);
    return UNITY_END();
}
