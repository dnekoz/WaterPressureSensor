#include "Logic.h"

#include <ArduinoJson.h>

#include <stdio.h>
#include <string.h>

// Версия прошивки попадает в discovery-payload. Config.h сюда не включаем:
// он тянет Arduino-специфику, а этот модуль должен собираться и на хосте.
#ifndef FW_VERSION
#define FW_VERSION "2.0.0"
#endif

// Копирует строку в поле фиксированной длины, всегда оставляя завершающий ноль.
static void copyField(char *dst, size_t size, const char *src) {
  strncpy(dst, src ? src : "", size - 1);
  dst[size - 1] = '\0';
}

// ---------------------------------------------------------------- настройки ---

void settingsDefaults(Settings &s, const char *deviceId) {
  copyField(s.mqttHost, sizeof(s.mqttHost), "");
  s.mqttPort = 1883;
  copyField(s.mqttUser, sizeof(s.mqttUser), "");
  copyField(s.mqttPass, sizeof(s.mqttPass), "");

  char base[64];
  snprintf(base, sizeof(base), "watersensor/%s", deviceId ? deviceId : "device");
  copyField(s.mqttBase, sizeof(s.mqttBase), base);

  s.mqttIntervalS = 10;
  copyField(s.haPrefix, sizeof(s.haPrefix), "homeassistant");
  copyField(s.deviceName, sizeof(s.deviceName), "Water Pressure");

  s.adcFullScaleV = 3.2f;  // делитель 220k/100k на плате D1 mini
  s.vZero         = 0.5f;  // XIDIBEI: 0.5 В при нулевом давлении
  s.vFull         = 4.5f;  // XIDIBEI: 4.5 В при полной шкале
  s.pFullMPa      = 0.5f;  // полная шкала конкретного датчика
}

bool settingsSanitize(Settings &s, const char *deviceId) {
  bool fixed = false;

  if (s.mqttPort == 0) {
    s.mqttPort = 1883;
    fixed = true;
  }
  if (s.mqttIntervalS < 1 || s.mqttIntervalS > 3600) {
    s.mqttIntervalS = 10;
    fixed = true;
  }
  // haPrefix намеренно не проверяется: пустая строка — штатный способ
  // выключить discovery Home Assistant, а не ошибка.
  if (s.deviceName[0] == '\0') {
    copyField(s.deviceName, sizeof(s.deviceName), "Water Pressure");
    fixed = true;
  }
  if (s.mqttBase[0] == '\0') {
    char base[64];
    snprintf(base, sizeof(base), "watersensor/%s", deviceId ? deviceId : "device");
    copyField(s.mqttBase, sizeof(s.mqttBase), base);
    fixed = true;
  }
  // Хвостовой '/' дал бы топик вида "base//state".
  size_t baseLen = strlen(s.mqttBase);
  while (baseLen > 0 && s.mqttBase[baseLen - 1] == '/') {
    s.mqttBase[--baseLen] = '\0';
    fixed = true;
  }
  // Слэши могли оказаться единственным содержимым — тогда база опустела.
  if (s.mqttBase[0] == '\0') {
    char base[64];
    snprintf(base, sizeof(base), "watersensor/%s", deviceId ? deviceId : "device");
    copyField(s.mqttBase, sizeof(s.mqttBase), base);
    fixed = true;
  }

  // Условия записаны через отрицание, чтобы NaN тоже считался невалидным:
  // любое сравнение с NaN ложно, поэтому !(...) даёт true и поле сбрасывается.
  if (!(s.adcFullScaleV >= 1.0f && s.adcFullScaleV <= 5.0f)) {
    s.adcFullScaleV = 3.2f;
    fixed = true;
  }
  if (!(s.pFullMPa > 0.0f)) {
    s.pFullMPa = 0.5f;
    fixed = true;
  }
  // Ниже этой проверки pressureConvert() делит на (vFull - vZero).
  if (!(s.vFull - s.vZero > 0.1f)) {
    s.vZero = 0.5f;
    s.vFull = 4.5f;
    fixed = true;
  }

  return fixed;
}

// ----------------------------------------------------------------- измерение ---

Reading pressureConvert(int raw, const Settings &s) {
  Reading r;

  r.raw   = raw;
  r.volts = (float)raw / 1023.0f * s.adcFullScaleV;

  // Знаменатель гарантированно > 0.1 — см. settingsSanitize().
  r.mpa = (r.volts - s.vZero) * s.pFullMPa / (s.vFull - s.vZero);
  r.bar = r.mpa * 10.0f;

  r.over  = raw >= SENSOR_RAW_OVER;
  r.fault = r.volts < (s.vZero - SENSOR_FAULT_MARGIN_V);

  return r;
}

float pressureMaxBar(const Settings &s) {
  float maxMpa = (s.adcFullScaleV - s.vZero) * s.pFullMPa / (s.vFull - s.vZero);
  return maxMpa * 10.0f;
}

// ---------------------------------------------------------------------- MQTT ---

void mqttTopic(char *out, size_t size, const Settings &s, const char *suffix) {
  snprintf(out, size, "%s/%s", s.mqttBase, suffix);
}

void mqttDiscoveryTopic(char *out, size_t size, const Settings &s, const char *deviceId,
                        const char *key) {
  snprintf(out, size, "%s/sensor/%s/%s/config", s.haPrefix, deviceId, key);
}

size_t mqttStateJson(char *out, size_t size, const Reading &r, int rssi, uint32_t heap) {
  JsonDocument doc;

  // Буферы живут до конца функции, то есть до сериализации.
  char barBuf[16];
  char mpaBuf[16];
  char voltBuf[16];

  // При over/fault давление неизвестно — публикуем null, чтобы Home Assistant
  // не рисовал на графике заведомо неверное число. Напряжение и сырой отсчёт
  // остаются: они измерены честно и как раз помогают понять причину.
  if (r.over || r.fault) {
    doc["bar"] = nullptr;
    doc["mpa"] = nullptr;
  } else {
    snprintf(barBuf, sizeof(barBuf), "%.2f", r.bar);
    snprintf(mpaBuf, sizeof(mpaBuf), "%.3f", r.mpa);
    doc["bar"] = serialized(barBuf);
    doc["mpa"] = serialized(mpaBuf);
  }

  snprintf(voltBuf, sizeof(voltBuf), "%.3f", r.volts);
  doc["volt"] = serialized(voltBuf);

  doc["raw"]   = r.raw;
  doc["rssi"]  = rssi;
  doc["over"]  = r.over;
  doc["fault"] = r.fault;
  doc["heap"]  = heap;

  return serializeJson(doc, out, size);
}

size_t mqttDiscoveryJson(char *out, size_t size, const Settings &s, const char *deviceId,
                         const char *key, const char *name, const char *devClass,
                         const char *unit, const char *valueTemplate, bool diagnostic) {
  char stateTopic[96];
  char availTopic[96];
  mqttTopic(stateTopic, sizeof(stateTopic), s, "state");
  mqttTopic(availTopic, sizeof(availTopic), s, "status");

  char uniqueId[64];
  snprintf(uniqueId, sizeof(uniqueId), "%s_%s", deviceId, key);

  // Ключи сокращённые — это штатные псевдонимы Home Assistant, они заметно
  // экономят место в буфере публикации PubSubClient.
  JsonDocument doc;
  doc["name"]    = name;          // имя сущности
  doc["uniq_id"] = uniqueId;      // unique_id
  doc["stat_t"]  = stateTopic;    // state_topic
  doc["avty_t"]  = availTopic;    // availability_topic
  doc["val_tpl"] = valueTemplate; // value_template
  if (devClass) doc["dev_cla"] = devClass;
  if (unit) doc["unit_of_meas"] = unit;
  doc["stat_cla"] = "measurement";
  if (diagnostic) doc["ent_cat"] = "diagnostic";

  // Общий блок device склеивает все сущности в одно устройство в интерфейсе HA.
  JsonObject dev = doc["dev"].to<JsonObject>();
  dev["ids"].to<JsonArray>().add(deviceId);
  dev["name"] = s.deviceName;
  dev["mdl"]  = "Wemos D1 mini";
  dev["mf"]   = "Espressif";
  dev["sw"]   = FW_VERSION;

  return serializeJson(doc, out, size);
}
