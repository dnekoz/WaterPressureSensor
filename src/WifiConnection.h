#pragma once

#include <stdint.h>

// Восстановление STA не зависит от времени жизни резервной точки доступа.
// Между выключением STA и новой попыткой loop() продолжает обслуживать устройство.
class WifiConnection {
public:
  void begin(uint32_t now);
  void connect(const char *ssid, const char *password, uint32_t now);
  void loop(uint32_t now);
  bool connected() const;
  bool configured() const { return s_ssid[0] != '\0'; }
  const char *ssid() const { return s_ssid; }
  const char *password() const { return s_password; }

private:
  void prepareAttempt(uint32_t now, bool save);
  void startAttempt(bool save);
  char s_ssid[33] = {};
  char s_password[65] = {};
  uint32_t s_lastAttemptMs = 0;
  uint32_t s_stationOffMs = 0;
  bool s_restartPending = false;
  bool s_savePending = false;
};
