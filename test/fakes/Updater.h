#pragma once

#include <stddef.h>
#include <stdint.h>
#include <new>

constexpr int U_FLASH = 0;
struct FakeUpdaterState {
  unsigned buffers = 0;
  unsigned commits = 0;
  unsigned cancellations = 0;
  bool failBegin = false;
  bool failWrite = false;
  bool failEnd = false;
};
inline FakeUpdaterState &fakeUpdater() { static FakeUpdaterState state; return state; }

// Model the relevant Core 3.1.2 contract, including its non-owning destructor
// and end(false) committing when all capacity bytes have already been written.
class UpdaterClass {
public:
  ~UpdaterClass() = default;
  void runAsync(bool) {}
  bool begin(size_t size, int) {
    if (fakeUpdater().failBegin) return false;
    s_buffer = new (std::nothrow) uint8_t[4096];
    if (!s_buffer) return false;
    ++fakeUpdater().buffers;
    s_size = size;
    return true;
  }
  bool isRunning() const { return s_size != 0; }
  size_t write(uint8_t *, size_t length) {
    if (fakeUpdater().failWrite || length > s_size - s_received) { reset(); return 0; }
    s_received += length;
    return length;
  }
  bool end(bool allowRemaining) {
    if (!s_size) return false;
    if (fakeUpdater().failEnd || (!allowRemaining && s_received != s_size)) {
      ++fakeUpdater().cancellations;
      reset();
      return false;
    }
    ++fakeUpdater().commits;
    reset();
    return true;
  }

private:
  void reset() {
    if (s_buffer) { delete[] s_buffer; --fakeUpdater().buffers; }
    s_buffer = nullptr;
    s_size = s_received = 0;
  }
  uint8_t *s_buffer = nullptr;
  size_t s_size = 0;
  size_t s_received = 0;
};
