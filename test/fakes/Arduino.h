#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define F(s) s
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
