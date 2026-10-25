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
  s_savePending = false;
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  if (configured()) prepareAttempt(now, false);
}

void WifiConnection::connect(const char *ssid, const char *password, uint32_t now) {
  snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
  snprintf(s_password, sizeof(s_password), "%s", password ? password : "");
  // Новые настройки отменяют отложенную попытку со старой сетью.
  s_restartPending = false;
  prepareAttempt(now, true);
}

bool WifiConnection::connected() const {
  return !s_restartPending && (WiFi.getMode() & WIFI_STA) && WiFi.status() == WL_CONNECTED &&
         uint32_t(WiFi.localIP()) != 0;
}

void WifiConnection::prepareAttempt(uint32_t now, bool save) {
  s_lastAttemptMs = now;
  s_savePending = s_savePending || save;
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  // Подход Tasmota: очищаем текущую конфигурацию SDK перед новым begin().
  // false сохраняет AP, true очищает только RAM при persistent(false).
  // SSID/пароль уже скопированы в поля объекта до вызова disconnect().
  const bool disconnected = WiFi.disconnect(false, true);
  const bool stopped = WiFi.enableSTA(false);
  // Смена режима SDK может занимать время. Отсчитываем паузу ПОСЛЕ неё.
  s_stationOffMs = millis();
  s_lastAttemptMs = s_stationOffMs;
  s_restartPending = stopped;
  if (!stopped) {
    WiFi.setAutoReconnect(true);
    Serial.println(F("[WIFI][ERROR] Не удалось выключить STA. Повтор через 30 с."));
    return;
  }
  if (!disconnected) {
    Serial.println(F("[WIFI][RETRY] SDK отклонил disconnect; STA выключен для новой попытки."));
  }
  Serial.printf("[WIFI][PREPARE] STA выключен, пауза %lu мс; конфигурация flash сохранена.\n",
                static_cast<unsigned long>(WIFI_STA_RESTART_PAUSE_MS));
}

void WifiConnection::startAttempt(bool save) {
  if (save) {
    // Core compares against flash in persistent mode and can skip the setter
    // for an unchanged profile. Restore RAM first (without connecting), since
    // prepareAttempt() cleared it even when the saved profile is identical.
    WiFi.persistent(false);
    WiFi.begin(s_ssid, s_password, 0, nullptr, false);
  }
  // Flash обновляется только при сохранении сети с портала. Канал 0 и отсутствие
  // BSSID разрешают новый поиск, в том числе после смены канала роутером.
  WiFi.persistent(save);
  const auto status = WiFi.begin(s_ssid, s_password, 0, nullptr);
  WiFi.persistent(false);
  s_lastAttemptMs = millis();
  s_savePending = false;
  Serial.printf("[WIFI][CONNECT] Сеть: %s, статус SDK: %d. Ожидаю подключения.\n",
                s_ssid, int(status));
}

void WifiConnection::loop(uint32_t now) {
  if (s_restartPending) {
    if (uint32_t(now - s_stationOffMs) < WIFI_STA_RESTART_PAUSE_MS) return;
    Serial.printf("[WIFI][RESUME] После отключения STA прошло %lu мс.\n",
                  static_cast<unsigned long>(uint32_t(now - s_stationOffMs)));
    s_restartPending = false;
    if (!WiFi.enableSTA(true)) {
      WiFi.setAutoReconnect(true);
      s_lastAttemptMs = millis();
      Serial.println(F("[WIFI][ERROR] Не удалось включить STA. Повтор через 30 с."));
      return;
    }
    WiFi.setAutoReconnect(true);
    startAttempt(s_savePending);
    return;
  }

  if (connected()) {
    // Отсчёт новой потери связи начинается от последнего здорового цикла.
    s_lastAttemptMs = now;
    return;
  }
  if (!configured()) return;

  if (uint32_t(now - s_lastAttemptMs) < WIFI_RECONNECT_MS) return;
  Serial.printf("[WIFI][RETRY] Связи нет 30 с. Перезапускаю STA; статус SDK: %d, режим: %d.\n",
                int(WiFi.status()), int(WiFi.getMode()));
  prepareAttempt(now, s_savePending);
}
