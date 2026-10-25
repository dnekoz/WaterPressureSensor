#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define F(s) s
inline uint32_t &fakeMillisValue() { static uint32_t value = 0; return value; }
inline unsigned long millis() { return fakeMillisValue(); }
inline void setFakeMillis(uint32_t value) { fakeMillisValue() = value; }
inline void advanceFakeMillis(uint32_t delta) { fakeMillisValue() += delta; }
struct FakeSerial {
  template<class... Args> void printf(const char *, Args...) {}
  void println(const char *) {}
};
static FakeSerial Serial;
struct FakeEsp {
  uint32_t getChipId() const { return 0xabc123; }
  uint32_t random() const { return 0x12345678; }
};
static FakeEsp ESP;
