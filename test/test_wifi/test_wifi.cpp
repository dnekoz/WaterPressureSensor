#include <unity.h>
#include <ESP8266WiFi.h>
#include "WifiConnection.h"
#include "Config.h"
#include "../../src/WifiConnection.cpp"

FakeWiFiClass WiFi;
static WifiConnection connection;
static constexpr uint32_t FIRST_ATTEMPT = WIFI_STA_RESTART_PAUSE_MS;
static constexpr uint32_t FIRST_RETRY = FIRST_ATTEMPT + WIFI_RECONNECT_MS;

static void beginAt(uint32_t now) { setFakeMillis(now); connection.begin(now); }
static void loopAt(uint32_t now) { setFakeMillis(now); connection.loop(now); }
static void connectAt(const char *ssid, const char *password, uint32_t now) {
  setFakeMillis(now); connection.connect(ssid, password, now);
}
static void boot() { beginAt(0); loopAt(FIRST_ATTEMPT); }
void setUp() { (void)ESP; setFakeMillis(0); WiFi = FakeWiFiClass{}; connection = WifiConnection{}; }
void tearDown() {}

static void test_saved_network_clears_ram_and_waits_200ms_without_flash_write() {
  TEST_ASSERT_EQUAL_UINT(200, WIFI_STA_RESTART_PAUSE_MS);
  beginAt(0);
  TEST_ASSERT_TRUE(connection.configured());
  TEST_ASSERT_EQUAL_STRING("Home", connection.ssid());
  TEST_ASSERT_EQUAL_STRING("test-password", connection.password());
  TEST_ASSERT_TRUE(WiFi.currentSsid.empty());
  TEST_ASSERT_TRUE(WiFi.currentPassword.empty());
  TEST_ASSERT_EQUAL_STRING("Home", WiFi.savedSsid.c_str());
  TEST_ASSERT_EQUAL_STRING("test-password", WiFi.savedPassword.c_str());
  TEST_ASSERT_EQUAL_UINT(0, WiFi.flashWrites);
  TEST_ASSERT_EQUAL_UINT(0, WiFi.attempts.size());
  loopAt(199);
  TEST_ASSERT_EQUAL_UINT(0, WiFi.attempts.size());
  loopAt(200);
  TEST_ASSERT_EQUAL_UINT(1, WiFi.attempts.size());
  TEST_ASSERT_EQUAL_STRING("Home", WiFi.attempts.back().ssid.c_str());
  TEST_ASSERT_EQUAL_STRING("test-password", WiFi.attempts.back().password.c_str());
  TEST_ASSERT_EQUAL_INT(0, WiFi.attempts.back().channel);
  TEST_ASSERT_FALSE(WiFi.attempts.back().bssidPinned);
  TEST_ASSERT_FALSE(WiFi.attempts.back().saved);
  TEST_ASSERT_TRUE(WiFi.autoReconnect);
  TEST_ASSERT_FALSE(WiFi.persistentWrites);
}

static void test_pause_starts_after_sdk_has_finished_disabling_station() {
  WiFi.disableDurationMs = 700;
  beginAt(0);
  TEST_ASSERT_EQUAL_UINT(700, millis());
  loopAt(899);
  TEST_ASSERT_EQUAL_UINT(0, WiFi.attempts.size());
  loopAt(900);
  TEST_ASSERT_EQUAL_UINT(1, WiFi.attempts.size());
}

static void test_router_returns_after_long_outage_even_after_ap_timeout() {
  WiFi.routerAvailable = true;
  boot(); loopAt(1000);
  WiFi.routerAvailable = false; WiFi.state = WL_NO_SSID_AVAIL; WiFi.ip = 0;
  WiFi.mode = WIFI_AP_STA;
  loopAt(30999);
  TEST_ASSERT_EQUAL_UINT(1, WiFi.attempts.size());
  loopAt(31000);
  TEST_ASSERT_EQUAL_INT(WIFI_AP, WiFi.mode);
  loopAt(31199);
  TEST_ASSERT_EQUAL_UINT(1, WiFi.attempts.size());
  loopAt(31200);
  TEST_ASSERT_EQUAL_INT(WIFI_AP_STA, WiFi.mode);
  TEST_ASSERT_EQUAL_UINT(2, WiFi.attempts.size());
  WiFi.mode = WIFI_STA; // Портал закрыл резервную AP.
  for (uint32_t now = 61200; now <= 212200; now += 30200) {
    loopAt(now); loopAt(now + WIFI_STA_RESTART_PAUSE_MS);
  }
  WiFi.routerAvailable = true;
  loopAt(300000); loopAt(300200);
  TEST_ASSERT_TRUE(connection.connected());
  TEST_ASSERT_EQUAL_STRING("Home", WiFi.attempts.back().ssid.c_str());
  TEST_ASSERT_EQUAL_STRING("test-password", WiFi.attempts.back().password.c_str());
  TEST_ASSERT_EQUAL_UINT(0, WiFi.flashWrites);
  TEST_ASSERT_EQUAL_STRING("Home", WiFi.savedSsid.c_str());
}

static void test_sdk_can_recover_without_additional_station_restart() {
  boot(); loopAt(12000);
  WiFi.state = WL_CONNECTED; WiFi.ip = 0x4501a8c0;
  loopAt(15000); loopAt(1200000);
  TEST_ASSERT_EQUAL_UINT(1, WiFi.stationStops); // Только начальная очистка.
  TEST_ASSERT_EQUAL_UINT(1, WiFi.attempts.size());
}

static void test_new_profile_is_applied_after_pause_despite_stale_connected_status() {
  boot(); connectAt("New router", "new-password", 1000);
  WiFi.mode = WIFI_STA; WiFi.state = WL_CONNECTED; WiFi.ip = 0x4501a8c0;
  loopAt(1199);
  TEST_ASSERT_EQUAL_UINT(1, WiFi.attempts.size());
  TEST_ASSERT_FALSE(WiFi.autoReconnect);
  TEST_ASSERT_FALSE(connection.connected());
  loopAt(1200);
  TEST_ASSERT_EQUAL_UINT(2, WiFi.attempts.size());
  TEST_ASSERT_EQUAL_STRING("New router", WiFi.savedSsid.c_str());
  TEST_ASSERT_EQUAL_UINT(1, WiFi.flashWrites);
  TEST_ASSERT_TRUE(WiFi.autoReconnect);
}

static void test_new_outage_gets_full_attempt_window_after_long_uptime() {
  WiFi.routerAvailable = true;
  boot(); loopAt(1000000);
  WiFi.routerAvailable = false; WiFi.state = WL_DISCONNECTED; WiFi.ip = 0;
  loopAt(1000001); loopAt(1029999);
  TEST_ASSERT_EQUAL_UINT(1, WiFi.stationStops);
  loopAt(1030000);
  TEST_ASSERT_EQUAL_UINT(2, WiFi.stationStops);
}

static void test_no_credentials_keeps_setup_ap_untouched() {
  WiFi.currentSsid.clear(); WiFi.currentPassword.clear(); WiFi.mode = WIFI_AP_STA;
  beginAt(0); loopAt(3600000);
  TEST_ASSERT_FALSE(connection.configured());
  TEST_ASSERT_EQUAL_UINT(0, WiFi.attempts.size());
  TEST_ASSERT_EQUAL_UINT(0, WiFi.stationStops);
  TEST_ASSERT_EQUAL_UINT(0, WiFi.disconnects);
  TEST_ASSERT_EQUAL_INT(WIFI_AP_STA, WiFi.mode);
}

static void test_new_credentials_cancel_old_pending_retry_and_save_once() {
  boot(); loopAt(FIRST_RETRY);
  connectAt("New router", "new-password", FIRST_RETRY + 50);
  loopAt(FIRST_RETRY + 200);
  TEST_ASSERT_EQUAL_UINT(1, WiFi.attempts.size());
  loopAt(FIRST_RETRY + 250);
  TEST_ASSERT_EQUAL_UINT(2, WiFi.attempts.size());
  TEST_ASSERT_EQUAL_STRING("New router", connection.ssid());
  TEST_ASSERT_TRUE(WiFi.attempts.back().saved);
  loopAt(FIRST_RETRY + 250 + WIFI_RECONNECT_MS);
  loopAt(FIRST_RETRY + 450 + WIFI_RECONNECT_MS);
  TEST_ASSERT_EQUAL_UINT(3, WiFi.attempts.size());
  TEST_ASSERT_EQUAL_STRING("new-password", WiFi.attempts.back().password.c_str());
  TEST_ASSERT_FALSE(WiFi.attempts.back().saved);
  TEST_ASSERT_EQUAL_UINT(1, WiFi.flashWrites);
  TEST_ASSERT_FALSE(WiFi.persistentWrites);
}

static void test_credentials_survive_missing_sdk_config_and_open_network() {
  boot(); WiFi.currentSsid.clear(); WiFi.currentPassword.clear();
  loopAt(FIRST_RETRY); loopAt(FIRST_RETRY + 200);
  TEST_ASSERT_EQUAL_STRING("Home", WiFi.attempts.back().ssid.c_str());
  TEST_ASSERT_EQUAL_STRING("test-password", WiFi.attempts.back().password.c_str());
  connectAt("Open", "", 40000); loopAt(40200); loopAt(70200); loopAt(70400);
  TEST_ASSERT_EQUAL_STRING("Open", WiFi.attempts.back().ssid.c_str());
  TEST_ASSERT_EQUAL_STRING("", WiFi.attempts.back().password.c_str());
  TEST_ASSERT_EQUAL_STRING("Open", WiFi.savedSsid.c_str());
  TEST_ASSERT_EQUAL_UINT(1, WiFi.flashWrites);
}

static void test_retry_and_restart_pause_survive_millis_wrap() {
  const uint32_t retry = UINT32_MAX - 50;
  const uint32_t first = retry - WIFI_RECONNECT_MS;
  beginAt(first - WIFI_STA_RESTART_PAUSE_MS); loopAt(first); loopAt(retry);
  loopAt(uint32_t(retry + WIFI_STA_RESTART_PAUSE_MS - 1));
  TEST_ASSERT_EQUAL_UINT(1, WiFi.attempts.size());
  loopAt(uint32_t(retry + WIFI_STA_RESTART_PAUSE_MS));
  TEST_ASSERT_EQUAL_UINT(2, WiFi.attempts.size());
  loopAt(uint32_t(retry + WIFI_STA_RESTART_PAUSE_MS + WIFI_RECONNECT_MS - 1));
  TEST_ASSERT_EQUAL_UINT(2, WiFi.stationStops);
  loopAt(uint32_t(retry + WIFI_STA_RESTART_PAUSE_MS + WIFI_RECONNECT_MS));
  TEST_ASSERT_EQUAL_UINT(3, WiFi.stationStops);
}

static void test_station_mode_failures_are_retried_with_auto_reconnect_enabled() {
  boot(); WiFi.failDisable = true; loopAt(FIRST_RETRY);
  TEST_ASSERT_TRUE(WiFi.autoReconnect);
  TEST_ASSERT_EQUAL_UINT(1, WiFi.attempts.size());
  WiFi.failDisable = false; loopAt(FIRST_RETRY + WIFI_RECONNECT_MS);
  WiFi.failEnable = true; loopAt(FIRST_RETRY + WIFI_RECONNECT_MS + 200);
  TEST_ASSERT_TRUE(WiFi.autoReconnect);
  TEST_ASSERT_EQUAL_UINT(1, WiFi.attempts.size());
  WiFi.failEnable = false; WiFi.routerAvailable = true;
  loopAt(100000); loopAt(100200);
  TEST_ASSERT_TRUE(connection.connected());
}

static void test_failed_sdk_disconnect_does_not_stop_recovery() {
  boot(); WiFi.failDisconnect = true; loopAt(FIRST_RETRY);
  WiFi.routerAvailable = true; loopAt(FIRST_RETRY + 200);
  TEST_ASSERT_TRUE(connection.connected());
  TEST_ASSERT_EQUAL_UINT(0, WiFi.flashWrites);
}

static void test_connected_status_without_ip_does_not_stop_recovery() {
  boot(); WiFi.state = WL_CONNECTED; WiFi.ip = 0;
  TEST_ASSERT_FALSE(connection.connected());
  loopAt(FIRST_RETRY);
  TEST_ASSERT_EQUAL_UINT(2, WiFi.stationStops);
  WiFi.routerAvailable = true; loopAt(FIRST_RETRY + 200);
  TEST_ASSERT_TRUE(connection.connected());
}

static void test_profile_save_survives_failed_disable_and_enable() {
  boot(); WiFi.failDisable = true; connectAt("New router", "new-password", 1000);
  TEST_ASSERT_EQUAL_UINT(0, WiFi.flashWrites);
  WiFi.failDisable = false; loopAt(31000);
  WiFi.failEnable = true; loopAt(31200);
  TEST_ASSERT_EQUAL_UINT(0, WiFi.flashWrites);
  WiFi.failEnable = false; loopAt(61200); loopAt(61400);
  TEST_ASSERT_EQUAL_STRING("New router", WiFi.savedSsid.c_str());
  TEST_ASSERT_EQUAL_STRING("new-password", WiFi.savedPassword.c_str());
  TEST_ASSERT_EQUAL_UINT(1, WiFi.flashWrites);
}

static void test_same_saved_profile_is_restored_even_when_flash_write_is_skipped() {
  WiFi.routerAvailable = true;
  boot();
  connectAt("Home", "test-password", 1000);
  TEST_ASSERT_TRUE(WiFi.currentSsid.empty());
  loopAt(1200);
  TEST_ASSERT_TRUE(connection.connected());
  TEST_ASSERT_EQUAL_STRING("Home", WiFi.currentSsid.c_str());
  TEST_ASSERT_EQUAL_STRING("test-password", WiFi.currentPassword.c_str());
  TEST_ASSERT_EQUAL_UINT(0, WiFi.flashWrites);
}

static void test_closing_ap_preserves_station_off_pause() {
  boot(); WiFi.mode = WIFI_AP_STA; loopAt(FIRST_RETRY);
  TEST_ASSERT_EQUAL_INT(WIFI_AP, WiFi.mode);
  WiFi.mode = WIFI_OFF; // softAPdisconnect(true) снимает AP-бит.
  loopAt(FIRST_RETRY + 199);
  TEST_ASSERT_EQUAL_INT(WIFI_OFF, WiFi.mode);
  TEST_ASSERT_EQUAL_UINT(1, WiFi.attempts.size());
  loopAt(FIRST_RETRY + 200);
  TEST_ASSERT_EQUAL_INT(WIFI_STA, WiFi.mode);
  TEST_ASSERT_EQUAL_UINT(2, WiFi.attempts.size());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_saved_network_clears_ram_and_waits_200ms_without_flash_write);
  RUN_TEST(test_pause_starts_after_sdk_has_finished_disabling_station);
  RUN_TEST(test_router_returns_after_long_outage_even_after_ap_timeout);
  RUN_TEST(test_sdk_can_recover_without_additional_station_restart);
  RUN_TEST(test_new_profile_is_applied_after_pause_despite_stale_connected_status);
  RUN_TEST(test_new_outage_gets_full_attempt_window_after_long_uptime);
  RUN_TEST(test_no_credentials_keeps_setup_ap_untouched);
  RUN_TEST(test_new_credentials_cancel_old_pending_retry_and_save_once);
  RUN_TEST(test_credentials_survive_missing_sdk_config_and_open_network);
  RUN_TEST(test_retry_and_restart_pause_survive_millis_wrap);
  RUN_TEST(test_station_mode_failures_are_retried_with_auto_reconnect_enabled);
  RUN_TEST(test_failed_sdk_disconnect_does_not_stop_recovery);
  RUN_TEST(test_connected_status_without_ip_does_not_stop_recovery);
  RUN_TEST(test_profile_save_survives_failed_disable_and_enable);
  RUN_TEST(test_same_saved_profile_is_restored_even_when_flash_write_is_skipped);
  RUN_TEST(test_closing_ap_preserves_station_off_pause);
  return UNITY_END();
}
