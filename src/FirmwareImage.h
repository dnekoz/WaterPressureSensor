#pragma once

#include <stddef.h>
#include <stdint.h>

// Ordinary (not gzip) BIN produced by Arduino Core 3.1.2 tools/elf2bin.py.
// Its whole-image CRC treats the embedded size/CRC words as eight zero bytes.
// This detects damaged/truncated files; it is not a firmware signature.
class FirmwareImage {
public:
  static constexpr size_t SIZE_OFFSET = 4096 + 16;
  static constexpr size_t CRC_OFFSET = SIZE_OFFSET + 4;
  static constexpr size_t HEADER_END = CRC_OFFSET + 4;

  void begin(size_t limit) {
    s_limit = limit;
    s_received = 0;
    s_expectedSize = s_expectedCrc = 0;
    s_crc = 0xffffffffUL;
    s_error = nullptr;
  }

  bool write(const uint8_t *data, size_t length) {
    if (s_error) return false;
    if (length > s_limit - s_received) return fail("Образ превышает доступное место для обновления.");
    for (size_t i = 0; i < length; ++i) {
      const size_t offset = s_received++;
      uint8_t value = data[i];
      if ((offset == 0 || offset == 4096) && value != 0xe9)
        return fail("Нужен обычный firmware.bin Arduino Core 3.1.2, без gzip.");
      if (offset >= SIZE_OFFSET && offset < CRC_OFFSET)
        s_expectedSize |= uint32_t(value) << ((offset - SIZE_OFFSET) * 8);
      if (offset >= CRC_OFFSET && offset < HEADER_END)
        s_expectedCrc |= uint32_t(value) << ((offset - CRC_OFFSET) * 8);
      if (offset >= SIZE_OFFSET && offset < HEADER_END) value = 0;
      s_crc ^= uint32_t(value) << 24;
      for (uint8_t bit = 0; bit < 8; ++bit)
        s_crc = (s_crc & 0x80000000UL) ? (s_crc << 1) ^ 0x04c11db7UL : s_crc << 1;
      if (s_received == HEADER_END &&
          (s_expectedSize <= HEADER_END || s_expectedSize > s_limit))
        return fail("Недопустимый размер образа в заголовке прошивки.");
      if (s_received >= HEADER_END && s_received > s_expectedSize)
        return fail("В файле есть данные за пределами образа прошивки.");
    }
    return true;
  }

  bool finish() {
    if (s_error) return false;
    if (s_received < HEADER_END || s_received != s_expectedSize)
      return fail("Неполный образ прошивки: размер не совпадает с заголовком.");
    if (s_crc != s_expectedCrc) return fail("Повреждённый образ прошивки: CRC32 не совпадает.");
    return true;
  }

  const char *error() const { return s_error ? s_error : ""; }

private:
  bool fail(const char *message) { s_error = message; return false; }
  size_t s_limit = 0;
  size_t s_received = 0;
  uint32_t s_expectedSize = 0;
  uint32_t s_expectedCrc = 0;
  uint32_t s_crc = 0xffffffffUL;
  const char *s_error = nullptr;
};
