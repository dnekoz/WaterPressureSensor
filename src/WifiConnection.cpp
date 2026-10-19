#include "WifiConnection.h"

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <stdio.h>
#include "Config.h"

void WifiConnection::begin(uint32_t now) {
  // Копия нужна до перезапусков STA: повтор не должен зависеть от того,
  // какую конфигурацию SDK отдаёт во время потери связи или смены режима.
  const auto savedSsid = WiFi.SSID();
  const auto savedPassword = WiFi.psk();
  snprintf(s_ssid, sizeof(s_ssid), "%s", savedSsid.c_str());
  snprintf(s_password, sizeof(s_password), "%s", savedPassword.c_str());
  s_lastAttemptMs = now;
  s_restartPending = false;
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  if (configured()) startAttempt(now, false);
}

void WifiConnection::connect(const char *ssid, const char *password, uint32_t now) {
  snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
  snprintf(s_password, sizeof(s_password), "%s", password ? password : "");
  // Новые настройки отменяют отложенную попытку со старой сетью.
  s_restartPending = false;
  WiFi.setAutoReconnect(true);
  startAttempt(now, true);
}

void WifiConnection::startAttempt(uint32_t now, bool save) {
  s_lastAttemptMs = now;
  // Flash обновляется только при сохранении сети с портала. Канал 0 и отсутствие
  // BSSID разрешают новый поиск, в том числе после смены канала роутером.
  WiFi.persistent(save);
  const auto status = WiFi.begin(s_ssid, s_password, 0, nullptr);
  WiFi.persistent(false);
  Serial.printf("[WIFI][CONNECT] Сеть: %s, статус SDK: %d. Ожидаю подключения.\n",
                s_ssid, int(status));
}

void WifiConnection::loop(uint32_t now) {
  if (WiFi.status() == WL_CONNECTED) {
    if (s_restartPending) WiFi.setAutoReconnect(true);
    // Отсчёт новой потери связи начинается от последнего здорового цикла,
    // а не от запуска устройства или предыдущего отключения роутера.
    s_lastAttemptMs = now;
    s_restartPending = false;
    return;
  }
  if (!configured()) return;

  if (s_restartPending) {
    if (uint32_t(now - s_stationOffMs) < WIFI_STA_RESTART_PAUSE_MS) return;
    s_restartPending = false;
    s_lastAttemptMs = now;
    if (!WiFi.enableSTA(true)) {
      WiFi.setAutoReconnect(true);
      Serial.println(F("[WIFI][ERROR] Не удалось включить STA. Повтор через 30 с."));
      return;
    }
    WiFi.setAutoReconnect(true);
    startAttempt(now, false);
    return;
  }

  if (uint32_t(now - s_lastAttemptMs) < WIFI_RECONNECT_MS) return;
  Serial.printf("[WIFI][RETRY] Связи нет 30 с. Перезапускаю STA; статус SDK: %d, режим: %d.\n",
                int(WiFi.status()), int(WiFi.getMode()));
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  // Не стираем SSID/пароль и не выключаем AP: телефон может быть на портале.
  const bool disconnected = WiFi.disconnect(false, false);
  if (!WiFi.enableSTA(false)) {
    WiFi.setAutoReconnect(true);
    s_lastAttemptMs = now;
    Serial.println(F("[WIFI][ERROR] Не удалось перезапустить STA. Повтор через 30 с."));
    return;
  }
  if (!disconnected) {
    Serial.println(F("[WIFI][RETRY] SDK отклонил disconnect; STA выключен для новой попытки."));
  }
  s_stationOffMs = now;
  s_restartPending = true;
}
