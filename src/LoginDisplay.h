#pragma once

#include <stdint.h>

class LoginDisplay {
public:
  static constexpr uint32_t TIMEOUT_MS = 60000;
  void show(uint32_t now) { s_visible = true; s_shownMs = now; }
  void hide() { s_visible = false; }
  bool visible(uint32_t now) {
    if (s_visible && uint32_t(now - s_shownMs) >= TIMEOUT_MS) hide();
    return s_visible;
  }

private:
  uint32_t s_shownMs = 0;
  bool s_visible = false;
};
