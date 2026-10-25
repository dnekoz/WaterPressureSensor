#include <unity.h>
#include <vector>
#include "FirmwareUpload.h"
#include "../../src/FirmwareUpload.cpp"

static constexpr size_t IMAGE_SIZE = 8208;
static constexpr size_t CAPACITY = 12288;

static std::vector<uint8_t> image() {
  std::vector<uint8_t> bytes(IMAGE_SIZE);
  for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = uint8_t(i * 37 + 11);
  bytes[0] = bytes[4096] = 0xe9;
  // Independently calculated with Core 3.1.2 tools/elf2bin.py::crc8266.
  const uint32_t words[] = {IMAGE_SIZE, 0x43b7a5a0UL};
  for (size_t i = 0; i < 8; ++i) bytes[FirmwareImage::SIZE_OFFSET + i] = uint8_t(words[i / 4] >> ((i % 4) * 8));
  return bytes;
}

void setUp() { fakeUpdater() = FakeUpdaterState{}; }
void tearDown() { TEST_ASSERT_EQUAL_UINT(0, fakeUpdater().buffers); }

static void test_complete_image_commits_once() {
  auto bytes = image();
  FirmwareUpload upload;
  TEST_ASSERT_TRUE(upload.begin(CAPACITY));
  for (size_t i = 0; i < bytes.size();) {
    const size_t count = bytes.size() - i < 137 ? bytes.size() - i : 137;
    TEST_ASSERT_TRUE(upload.write(bytes.data() + i, count));
    i += count;
  }
  TEST_ASSERT_EQUAL_UINT(0, fakeUpdater().commits);
  TEST_ASSERT_TRUE(upload.finish());
  TEST_ASSERT_EQUAL_UINT(1, fakeUpdater().commits);
  TEST_ASSERT_FALSE(upload.finish());
  TEST_ASSERT_EQUAL_UINT(1, fakeUpdater().commits);
}

static void test_header_words_split_at_every_byte() {
  auto bytes = image();
  for (size_t split = 4095; split <= FirmwareImage::HEADER_END + 1; ++split) {
    FirmwareUpload upload;
    TEST_ASSERT_TRUE(upload.begin(CAPACITY));
    TEST_ASSERT_TRUE(upload.write(bytes.data(), split));
    TEST_ASSERT_TRUE(upload.write(bytes.data() + split, bytes.size() - split));
    TEST_ASSERT_TRUE(upload.finish());
  }
}

static void test_truncated_images_never_commit() {
  auto bytes = image();
  const size_t lengths[] = {0, 1, 4096, 4115, 4119, 4120, IMAGE_SIZE - 1};
  for (size_t length : lengths) {
    FirmwareUpload upload;
    TEST_ASSERT_TRUE(upload.begin(CAPACITY));
    TEST_ASSERT_TRUE(upload.write(bytes.data(), length));
    TEST_ASSERT_FALSE(upload.finish());
    TEST_ASSERT_TRUE(upload.error()[0] != '\0');
    TEST_ASSERT_EQUAL_UINT(0, fakeUpdater().buffers);
    TEST_ASSERT_EQUAL_UINT(0, fakeUpdater().commits);
  }
}

static void test_corruption_anywhere_in_image_is_rejected() {
  const size_t offsets[] = {1, 100, 4095, 4097, 4112, 4116, 6000, IMAGE_SIZE - 1};
  for (size_t offset : offsets) {
    auto bytes = image(); bytes[offset] ^= 1;
    FirmwareUpload upload;
    TEST_ASSERT_TRUE(upload.begin(CAPACITY));
    const bool written = upload.write(bytes.data(), bytes.size());
    TEST_ASSERT_FALSE(written && upload.finish());
    TEST_ASSERT_EQUAL_UINT(0, fakeUpdater().buffers);
    TEST_ASSERT_EQUAL_UINT(0, fakeUpdater().commits);
  }
}

static void test_bad_magic_and_gzip_are_rejected() {
  for (size_t offset : {size_t(0), size_t(4096)}) {
    auto bytes = image(); bytes[offset] = 0x1f;
    FirmwareUpload upload;
    TEST_ASSERT_TRUE(upload.begin(CAPACITY));
    TEST_ASSERT_FALSE(upload.write(bytes.data(), bytes.size()));
    TEST_ASSERT_EQUAL_UINT(0, fakeUpdater().buffers);
    TEST_ASSERT_EQUAL_UINT(0, fakeUpdater().commits);
  }
}

static void test_trailing_data_is_rejected() {
  auto bytes = image(); bytes.push_back(0);
  FirmwareUpload upload;
  TEST_ASSERT_TRUE(upload.begin(CAPACITY));
  TEST_ASSERT_FALSE(upload.write(bytes.data(), bytes.size()));
  TEST_ASSERT_EQUAL_UINT(0, fakeUpdater().commits);
}

static void test_capacity_byte_is_never_accepted() {
  auto bytes = image();
  FirmwareUpload upload;
  TEST_ASSERT_TRUE(upload.begin(bytes.size()));
  TEST_ASSERT_FALSE(upload.write(bytes.data(), bytes.size()));
  TEST_ASSERT_EQUAL_UINT(0, fakeUpdater().commits);
  // A one-byte reserve is sufficient even when all valid bytes are buffered.
  TEST_ASSERT_TRUE(upload.begin(bytes.size() + 1));
  TEST_ASSERT_TRUE(upload.write(bytes.data(), bytes.size()));
  upload.abort();
  TEST_ASSERT_EQUAL_UINT(0, fakeUpdater().commits);
}

static void test_abort_after_complete_reception_does_not_commit() {
  auto bytes = image();
  FirmwareUpload upload;
  TEST_ASSERT_TRUE(upload.begin(CAPACITY));
  TEST_ASSERT_TRUE(upload.write(bytes.data(), bytes.size()));
  upload.abort(); upload.abort();
  TEST_ASSERT_FALSE(upload.finish());
  TEST_ASSERT_EQUAL_UINT(0, fakeUpdater().commits);
}

static void test_repeated_abort_and_restart_release_buffer() {
  auto bytes = image();
  FirmwareUpload upload;
  for (int i = 0; i < 20; ++i) {
    TEST_ASSERT_TRUE(upload.begin(CAPACITY));
    TEST_ASSERT_TRUE(upload.write(bytes.data(), 1024));
    upload.abort();
    TEST_ASSERT_EQUAL_UINT(0, fakeUpdater().buffers);
  }
  TEST_ASSERT_TRUE(upload.begin(CAPACITY));
  TEST_ASSERT_TRUE(upload.begin(CAPACITY));
  TEST_ASSERT_EQUAL_UINT(1, fakeUpdater().buffers);
  upload.abort();
  TEST_ASSERT_EQUAL_UINT(0, fakeUpdater().commits);
}

static void test_destructor_cancels_unfinished_upload() {
  auto bytes = image();
  { FirmwareUpload upload; TEST_ASSERT_TRUE(upload.begin(CAPACITY)); TEST_ASSERT_TRUE(upload.write(bytes.data(), 1024)); }
  TEST_ASSERT_EQUAL_UINT(0, fakeUpdater().buffers);
  TEST_ASSERT_EQUAL_UINT(0, fakeUpdater().commits);
}

static void test_begin_and_write_errors_release_buffer() {
  auto bytes = image();
  FirmwareUpload upload;
  TEST_ASSERT_FALSE(upload.begin(4096));
  fakeUpdater().failBegin = true;
  TEST_ASSERT_FALSE(upload.begin(CAPACITY));
  fakeUpdater().failBegin = false;
  TEST_ASSERT_TRUE(upload.begin(CAPACITY));
  fakeUpdater().failWrite = true;
  TEST_ASSERT_FALSE(upload.write(bytes.data(), 1024));
  TEST_ASSERT_FALSE(upload.finish());
  TEST_ASSERT_EQUAL_UINT(0, fakeUpdater().commits);
}

static void test_updater_commit_failure_is_reported() {
  auto bytes = image();
  FirmwareUpload upload;
  TEST_ASSERT_TRUE(upload.begin(CAPACITY));
  TEST_ASSERT_TRUE(upload.write(bytes.data(), bytes.size()));
  fakeUpdater().failEnd = true;
  TEST_ASSERT_FALSE(upload.finish());
  TEST_ASSERT_TRUE(upload.error()[0] != '\0');
  TEST_ASSERT_EQUAL_UINT(0, fakeUpdater().commits);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_complete_image_commits_once);
  RUN_TEST(test_header_words_split_at_every_byte);
  RUN_TEST(test_truncated_images_never_commit);
  RUN_TEST(test_corruption_anywhere_in_image_is_rejected);
  RUN_TEST(test_bad_magic_and_gzip_are_rejected);
  RUN_TEST(test_trailing_data_is_rejected);
  RUN_TEST(test_capacity_byte_is_never_accepted);
  RUN_TEST(test_abort_after_complete_reception_does_not_commit);
  RUN_TEST(test_repeated_abort_and_restart_release_buffer);
  RUN_TEST(test_destructor_cancels_unfinished_upload);
  RUN_TEST(test_begin_and_write_errors_release_buffer);
  RUN_TEST(test_updater_commit_failure_is_reported);
  return UNITY_END();
}
