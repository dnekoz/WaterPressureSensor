#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <string>
#include <vector>

enum wl_status_t { WL_IDLE_STATUS = 0, WL_NO_SSID_AVAIL = 1, WL_CONNECTED = 3,
                   WL_CONNECT_FAILED = 4, WL_WRONG_PASSWORD = 6, WL_DISCONNECTED = 7 };
enum WiFiMode_t { WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3 };

struct FakeWifiAttempt {
  std::string ssid, password;
  int channel = 0;
  bool bssidPinned = false, saved = false;
};

struct FakeWiFiClass {
  std::string currentSsid = "Home", currentPassword = "test-password";
  std::string savedSsid = "Home", savedPassword = "test-password";
  WiFiMode_t mode = WIFI_STA;
  wl_status_t state = WL_DISCONNECTED;
  bool persistentWrites = false, autoReconnect = true, routerAvailable = false;
  bool failDisable = false, failEnable = false, failDisconnect = false;
  unsigned erasedCredentials = 0, disconnects = 0, stationStops = 0;
  unsigned flashWrites = 0;
  uint32_t ip = 0, disableDurationMs = 0;
  std::vector<FakeWifiAttempt> attempts;

  const std::string &SSID() const { return currentSsid; }
  const std::string &psk() const { return currentPassword; }
  wl_status_t status() const { return state; }
  WiFiMode_t getMode() const { return mode; }
  uint32_t localIP() const { return ip; }
  void persistent(bool enabled) { persistentWrites = enabled; }
  bool setAutoReconnect(bool enabled) { autoReconnect = enabled; return true; }
  bool enableSTA(bool enabled) {
    if ((enabled && failEnable) || (!enabled && failDisable)) return false;
    mode = WiFiMode_t(enabled ? (mode | WIFI_STA) : (mode & ~WIFI_STA));
    if (!enabled) {
      ++stationStops; state = WL_DISCONNECTED; ip = 0;
      advanceFakeMillis(disableDurationMs);
    }
    return true;
  }
  bool disconnect(bool off, bool erase) {
    ++disconnects;
    if (erase) {
      ++erasedCredentials; currentSsid.clear(); currentPassword.clear();
      if (persistentWrites) { savedSsid.clear(); savedPassword.clear(); ++flashWrites; }
    }
    state = WL_DISCONNECTED;
    ip = 0;
    if (off) enableSTA(false);
    return !failDisconnect;
  }
  wl_status_t begin(const char *ssid, const char *password, int channel, const uint8_t *bssid,
                    bool connect = true) {
    // Core 3.1.2 compares against flash when persistence is enabled. If it
    // matches, the SDK setter is skipped, even if current RAM was cleared.
    if (!persistentWrites || savedSsid != ssid || savedPassword != password) {
      currentSsid = ssid;
      currentPassword = password;
      if (persistentWrites) { savedSsid = ssid; savedPassword = password; ++flashWrites; }
    }
    enableSTA(true);
    if (!connect) return state;
    attempts.push_back({ssid, password, channel, bssid != nullptr, persistentWrites});
    // Моделируем застрявший SDK: само появление роутера state не меняет;
    // новое подключение может восстановить его лишь при доступной сети.
    const bool linked = routerAvailable && currentSsid == ssid && currentPassword == password;
    ip = linked ? 0x4501a8c0 : 0;
    return state = linked ? WL_CONNECTED : WL_NO_SSID_AVAIL;
  }
};

extern FakeWiFiClass WiFi;
