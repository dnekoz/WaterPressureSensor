#include <unity.h>
#include <ESP8266WiFi.h>
#include "WifiConnection.h"
#include "Config.h"

// Как и тесты хранилища, собираем реальный модуль с подменой аппаратного API.
// Общий native-фильтр оставляет аппаратные модули вне остальных наборов тестов.
#include "../../src/WifiConnection.cpp"

FakeWiFiClass WiFi;
static WifiConnection connection;

void setUp() {
  (void)ESP; // Общая Arduino-подмена содержит также не нужный этому модулю ESP.
  WiFi = FakeWiFiClass{};
  connection = WifiConnection{};
}
void tearDown() {}

static void test_saved_network_starts_without_flash_write_or_channel_lock() {
  connection.begin(100);
  TEST_ASSERT_TRUE(connection.configured());
  TEST_ASSERT_EQUAL_STRING("Home", connection.ssid());
  TEST_ASSERT_EQUAL_UINT(1, WiFi.attempts.size());
  TEST_ASSERT_EQUAL_STRING("test-password", WiFi.attempts.back().password.c_str());
  TEST_ASSERT_EQUAL_INT(0, WiFi.attempts.back().channel);
  TEST_ASSERT_FALSE(WiFi.attempts.back().bssidPinned);
  TEST_ASSERT_FALSE(WiFi.attempts.back().saved);
  TEST_ASSERT_FALSE(WiFi.persistentWrites);
}

static void test_router_returns_after_long_outage_even_after_ap_timeout() {
  WiFi.routerAvailable = true;
  connection.begin(0);
  connection.loop(1000);
  WiFi.routerAvailable = false;
  WiFi.state = WL_NO_SSID_AVAIL;
  WiFi.mode = WIFI_AP_STA; // Резервная AP появилась после потери роутера.
  connection.loop(30999);
  TEST_ASSERT_EQUAL_UINT(1, WiFi.attempts.size());
  connection.loop(31000);
  TEST_ASSERT_EQUAL_INT(WIFI_AP, WiFi.mode);
  connection.loop(31099);
  TEST_ASSERT_EQUAL_UINT(1, WiFi.attempts.size());
  connection.loop(31100);
  TEST_ASSERT_EQUAL_INT(WIFI_AP_STA, WiFi.mode);
  TEST_ASSERT_EQUAL_UINT(2, WiFi.attempts.size());
  // Время жизни AP не является сроком прекращения повторных подключений.
  WiFi.mode = WIFI_STA; // Портал закрыл AP по своим 180 секундам.
  for (uint32_t now = 61200; now <= 211700; now += 30100) {
    connection.loop(now);
    connection.loop(now + WIFI_STA_RESTART_PAUSE_MS);
  }
  WiFi.routerAvailable = true;
  TEST_ASSERT_NOT_EQUAL(WL_CONNECTED, WiFi.state);
  connection.loop(242000);
  connection.loop(242100);
  TEST_ASSERT_EQUAL_INT(WL_CONNECTED, WiFi.state);
  TEST_ASSERT_EQUAL_UINT(0, WiFi.erasedCredentials);
  for (const auto &attempt : WiFi.attempts) TEST_ASSERT_FALSE(attempt.saved);
}

static void test_sdk_can_recover_without_station_restart() {
  connection.begin(0);
  connection.loop(12000);
  WiFi.state = WL_CONNECTED; // SDK подключился сам раньше 30 с.
  connection.loop(15000);
  connection.loop(1200000);
  TEST_ASSERT_EQUAL_UINT(0, WiFi.stationStops);
  TEST_ASSERT_EQUAL_UINT(1, WiFi.attempts.size());
}

static void test_connection_during_restart_restores_sdk_auto_reconnect() {
  connection.begin(0);
  connection.loop(30000);
  TEST_ASSERT_FALSE(WiFi.autoReconnect);
  // Закрытие резервной AP тоже может включить STA между этапами повтора.
  WiFi.mode = WIFI_STA;
  WiFi.state = WL_CONNECTED;
  connection.loop(30050);
  TEST_ASSERT_TRUE(WiFi.autoReconnect);
  connection.loop(30100);
  TEST_ASSERT_EQUAL_UINT(1, WiFi.attempts.size());
}

static void test_new_outage_gets_full_attempt_window_after_long_uptime() {
  WiFi.routerAvailable = true;
  connection.begin(0);
  connection.loop(1000000);
  WiFi.routerAvailable = false;
  WiFi.state = WL_DISCONNECTED;
  connection.loop(1000001);
  connection.loop(1029999);
  TEST_ASSERT_EQUAL_UINT(0, WiFi.stationStops);
  connection.loop(1030000);
  TEST_ASSERT_EQUAL_UINT(1, WiFi.stationStops);
}

static void test_no_credentials_keeps_setup_ap_untouched() {
  WiFi.currentSsid.clear(); WiFi.currentPassword.clear();
  WiFi.mode = WIFI_AP_STA;
  connection.begin(0);
  connection.loop(3600000);
  TEST_ASSERT_FALSE(connection.configured());
  TEST_ASSERT_EQUAL_UINT(0, WiFi.attempts.size());
  TEST_ASSERT_EQUAL_UINT(0, WiFi.stationStops);
  TEST_ASSERT_EQUAL_INT(WIFI_AP_STA, WiFi.mode);
}

static void test_new_credentials_cancel_old_pending_retry_and_save_once() {
  connection.begin(0);
  connection.loop(30000); // Старая сеть ожидает повторного включения STA.
  connection.connect("New router", "new-password", 30050);
  connection.loop(30100);
  TEST_ASSERT_EQUAL_UINT(2, WiFi.attempts.size());
  TEST_ASSERT_EQUAL_STRING("New router", connection.ssid());
  TEST_ASSERT_TRUE(WiFi.attempts.back().saved);
  TEST_ASSERT_FALSE(WiFi.persistentWrites);
  connection.loop(60050);
  connection.loop(60150);
  TEST_ASSERT_EQUAL_UINT(3, WiFi.attempts.size());
  TEST_ASSERT_EQUAL_STRING("New router", WiFi.attempts.back().ssid.c_str());
  TEST_ASSERT_EQUAL_STRING("new-password", WiFi.attempts.back().password.c_str());
  TEST_ASSERT_FALSE(WiFi.attempts.back().saved);
}

static void test_credentials_survive_missing_sdk_config_and_open_network() {
  connection.begin(0);
  WiFi.currentSsid.clear(); WiFi.currentPassword.clear();
  connection.loop(30000);
  connection.loop(30100);
  TEST_ASSERT_EQUAL_STRING("Home", WiFi.attempts.back().ssid.c_str());
  TEST_ASSERT_EQUAL_STRING("test-password", WiFi.attempts.back().password.c_str());
  connection.connect("Open", "", 40000);
  connection.loop(70000);
  connection.loop(70100);
  TEST_ASSERT_EQUAL_STRING("Open", WiFi.attempts.back().ssid.c_str());
  TEST_ASSERT_EQUAL_STRING("", WiFi.attempts.back().password.c_str());
}

static void test_retry_and_restart_pause_survive_millis_wrap() {
  const uint32_t start = UINT32_MAX - 50;
  connection.begin(start - WIFI_RECONNECT_MS);
  connection.loop(start);
  connection.loop(uint32_t(start + WIFI_STA_RESTART_PAUSE_MS - 1));
  TEST_ASSERT_EQUAL_UINT(1, WiFi.attempts.size());
  connection.loop(uint32_t(start + WIFI_STA_RESTART_PAUSE_MS));
  TEST_ASSERT_EQUAL_UINT(2, WiFi.attempts.size());
  connection.loop(uint32_t(start + WIFI_STA_RESTART_PAUSE_MS + WIFI_RECONNECT_MS - 1));
  TEST_ASSERT_EQUAL_UINT(1, WiFi.stationStops);
  connection.loop(uint32_t(start + WIFI_STA_RESTART_PAUSE_MS + WIFI_RECONNECT_MS));
  TEST_ASSERT_EQUAL_UINT(2, WiFi.stationStops);
}

static void test_station_mode_failures_are_retried_with_auto_reconnect_enabled() {
  connection.begin(0);
  WiFi.failDisable = true;
  connection.loop(30000);
  TEST_ASSERT_TRUE(WiFi.autoReconnect);
  TEST_ASSERT_EQUAL_UINT(1, WiFi.attempts.size());
  WiFi.failDisable = false;
  connection.loop(60000);
  WiFi.failEnable = true;
  connection.loop(60100);
  TEST_ASSERT_TRUE(WiFi.autoReconnect);
  TEST_ASSERT_EQUAL_UINT(1, WiFi.attempts.size());
  WiFi.failEnable = false;
  WiFi.routerAvailable = true;
  connection.loop(90100);
  connection.loop(90200);
  TEST_ASSERT_EQUAL_INT(WL_CONNECTED, WiFi.state);
}

static void test_failed_sdk_disconnect_does_not_stop_recovery() {
  connection.begin(0);
  WiFi.failDisconnect = true;
  connection.loop(30000);
  WiFi.routerAvailable = true;
  connection.loop(30100);
  TEST_ASSERT_EQUAL_INT(WL_CONNECTED, WiFi.state);
  TEST_ASSERT_EQUAL_UINT(0, WiFi.erasedCredentials);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_saved_network_starts_without_flash_write_or_channel_lock);
  RUN_TEST(test_router_returns_after_long_outage_even_after_ap_timeout);
  RUN_TEST(test_sdk_can_recover_without_station_restart);
  RUN_TEST(test_connection_during_restart_restores_sdk_auto_reconnect);
  RUN_TEST(test_new_outage_gets_full_attempt_window_after_long_uptime);
  RUN_TEST(test_no_credentials_keeps_setup_ap_untouched);
  RUN_TEST(test_new_credentials_cancel_old_pending_retry_and_save_once);
  RUN_TEST(test_credentials_survive_missing_sdk_config_and_open_network);
  RUN_TEST(test_retry_and_restart_pause_survive_millis_wrap);
  RUN_TEST(test_station_mode_failures_are_retried_with_auto_reconnect_enabled);
  RUN_TEST(test_failed_sdk_disconnect_does_not_stop_recovery);
  return UNITY_END();
}
