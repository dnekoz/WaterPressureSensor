#include "Logic.h"

#include <ArduinoJson.h>

#include <stdio.h>
#include <string.h>
#include <math.h>

// Версия прошивки попадает в discovery-payload. Config.h сюда не включаем: он
// тянет Arduino-специфику, а этот модуль должен собираться и на хосте. Значение
// приходит из build_flags обоих окружений — раньше здесь стояла своя копия
// константы, и в discovery уходила именно она, а не та, что в Config.h.
#ifndef FW_VERSION
#error "FW_VERSION не задана: соберите через PlatformIO, а не вручную"
#endif



// Копирует строку в поле фиксированной длины, всегда оставляя завершающий ноль.
static void copyField(char *dst, size_t size, const char *src) {
  if (!size) return;
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
  copyField(s.haPrefixApplied, sizeof(s.haPrefixApplied), "");
  copyField(s.portalPass, sizeof(s.portalPass), "");

  s.mqttTlsMode = TLS_OFF;
  copyField(s.mqttFingerprint, sizeof(s.mqttFingerprint), "");
  copyField(s.ntpServer, sizeof(s.ntpServer), "");

  s.adcFullScaleV = 3.2f;  // делитель 220k/100k на плате D1 mini
  s.vZero         = 0.5f;  // XIDIBEI: 0.5 В при нулевом давлении
  s.vFull         = 4.5f;  // XIDIBEI: 4.5 В при полной шкале
  s.pFullMPa      = 0.5f;  // полная шкала конкретного датчика
}

// Одна hex-цифра или -1.
static int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool parseFingerprint(const char *str, uint8_t out[TLS_FINGERPRINT_LEN]) {
  if (!str) return false;

  uint8_t buf[TLS_FINGERPRINT_LEN];
  size_t  n = 0;
  const char *p = str;

  while (*p && n < TLS_FINGERPRINT_LEN) {
    // Двоеточие допустимо только между байтами, а не перед первым и не сдвоенное.
    if (*p == ':') {
      if (n == 0) return false;
      p++;
    }

    int hi = hexDigit(p[0]);
    if (hi < 0) return false;
    int lo = hexDigit(p[1]);
    if (lo < 0) return false;

    buf[n++] = (uint8_t)((hi << 4) | lo);
    p += 2;
  }

  // Ровно 20 байт и ничего после них: и короткая, и длинная строка — ошибка
  // ввода, а молча дополненный нулями отпечаток не совпал бы ни с чем.
  if (n != TLS_FINGERPRINT_LEN || *p != '\0') return false;

  memcpy(out, buf, TLS_FINGERPRINT_LEN);
  return true;
}

bool settingsSanitize(Settings &s, const char *deviceId) {
  bool fixed = false;

  // Ошибки TLS не понижают защиту: подключение блокирует сетевой слой.

  if (s.mqttPort == 0) {
    // Порт по умолчанию зависит от режима: 8883 — штатный порт MQTT over TLS.
    // Заданный явно порт не трогаем, TLS на 1883 — законная конфигурация.
    s.mqttPort = (s.mqttTlsMode == TLS_OFF) ? 1883 : 8883;
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
  if (!(s.adcFullScaleV >= 1.0f && s.adcFullScaleV <= 10.0f)) {
    s.adcFullScaleV = 3.2f;
    fixed = true;
  }
  if (!(s.pFullMPa > 0.0f && s.pFullMPa <= 100.0f)) {
    s.pFullMPa = 0.5f;
    fixed = true;
  }
  // Ниже этой проверки pressureConvert() делит на (vFull - vZero).
  if (!isfinite(s.vZero) || !isfinite(s.vFull) || s.vZero < 0.0f || s.vFull > 10.0f ||
      !(s.vFull - s.vZero > 0.1f) || s.vZero >= s.adcFullScaleV) {
    s.vZero = 0.5f;
    s.vFull = 4.5f;
    fixed = true;
  }

  return fixed;
}

// ------------------------------------------------------ сериализация настроек ---

void settingsToJson(JsonObject dst, const Settings &s) {
  dst["mqttHost"]      = s.mqttHost;
  dst["mqttPort"]      = s.mqttPort;
  dst["mqttUser"]      = s.mqttUser;
  dst["mqttPass"]      = s.mqttPass;
  dst["mqttBase"]      = s.mqttBase;
  dst["mqttIntervalS"] = s.mqttIntervalS;
  dst["haPrefix"]        = s.haPrefix;
  dst["haPrefixApplied"] = s.haPrefixApplied;
  dst["deviceName"]      = s.deviceName;
  dst["portalPass"]      = s.portalPass;

  dst["mqttTlsMode"]     = s.mqttTlsMode;
  dst["mqttFingerprint"] = s.mqttFingerprint;
  dst["ntpServer"]       = s.ntpServer;

  dst["adcFullScaleV"] = s.adcFullScaleV;
  dst["vZero"]         = s.vZero;
  dst["vFull"]         = s.vFull;
  dst["pFullMPa"]      = s.pFullMPa;
}

// Перезаписывает поле, только если ключ реально есть в JSON и это строка: иначе
// оставляем прежнее значение (и не копируем строку саму в себя).
static void loadField(char *dst, size_t size, JsonVariantConst v) {
  if (v.is<const char *>()) copyField(dst, size, v.as<const char *>());
}

void settingsFromJson(JsonObjectConst src, Settings &s) {
  loadField(s.mqttHost, sizeof(s.mqttHost), src["mqttHost"]);
  s.mqttPort = src["mqttPort"] | s.mqttPort;
  loadField(s.mqttUser, sizeof(s.mqttUser), src["mqttUser"]);
  loadField(s.mqttPass, sizeof(s.mqttPass), src["mqttPass"]);
  loadField(s.mqttBase, sizeof(s.mqttBase), src["mqttBase"]);
  s.mqttIntervalS = src["mqttIntervalS"] | s.mqttIntervalS;
  loadField(s.haPrefix, sizeof(s.haPrefix), src["haPrefix"]);
  loadField(s.haPrefixApplied, sizeof(s.haPrefixApplied), src["haPrefixApplied"]);
  loadField(s.deviceName, sizeof(s.deviceName), src["deviceName"]);
  loadField(s.portalPass, sizeof(s.portalPass), src["portalPass"]);

  s.mqttTlsMode = src["mqttTlsMode"] | s.mqttTlsMode;
  loadField(s.mqttFingerprint, sizeof(s.mqttFingerprint), src["mqttFingerprint"]);
  loadField(s.ntpServer, sizeof(s.ntpServer), src["ntpServer"]);

  s.adcFullScaleV = src["adcFullScaleV"] | s.adcFullScaleV;
  s.vZero         = src["vZero"] | s.vZero;
  s.vFull         = src["vFull"] | s.vFull;
  s.pFullMPa      = src["pFullMPa"] | s.pFullMPa;
}

// ----------------------------------------------------------------- измерение ---

Reading pressureConvert(int raw, const Settings &s) {
  Reading r;

  r.raw   = raw;
  r.rawInstant = raw;
  r.volts = (float)raw / 1023.0f * s.adcFullScaleV;

  // Знаменатель гарантированно > 0.1 — см. settingsSanitize().
  r.mpa = (r.volts - s.vZero) * s.pFullMPa / (s.vFull - s.vZero);
  r.bar = r.mpa * 10.0f;

  r.over  = raw >= SENSOR_RAW_OVER || r.volts > s.vFull + 0.05f;
  r.fault = raw < 0 || r.volts < (s.vZero - SENSOR_FAULT_MARGIN_V) ||
            !isfinite(r.volts) || !isfinite(r.bar) || !isfinite(r.mpa);

  return r;
}

float pressureMaxBar(const Settings &s) {
  float maxMpa = (fminf(s.adcFullScaleV, s.vFull) - s.vZero) * s.pFullMPa / (s.vFull - s.vZero);
  return maxMpa * 10.0f;
}

// ---------------------------------------------------------------------- MQTT ---

void mqttTopic(char *out, size_t size, const Settings &s, const char *suffix) {
  snprintf(out, size, "%s/%s", s.mqttBase, suffix);
}

const char *const MQTT_ENTITY_KEYS[] = {"pressure", "voltage", "rssi", "raw"};
const size_t      MQTT_ENTITY_COUNT  = sizeof(MQTT_ENTITY_KEYS) / sizeof(MQTT_ENTITY_KEYS[0]);

void mqttDiscoveryTopicFor(char *out, size_t size, const char *haPrefix, const char *deviceId,
                           const char *key) {
  snprintf(out, size, "%s/sensor/%s/%s/config", haPrefix, deviceId, key);
}

void mqttDiscoveryTopic(char *out, size_t size, const Settings &s, const char *deviceId,
                        const char *key) {
  mqttDiscoveryTopicFor(out, size, s.haPrefix, deviceId, key);
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
  if (r.over || r.fault || r.stale || !isfinite(r.bar) || !isfinite(r.mpa)) {
    doc["bar"] = nullptr;
    doc["mpa"] = nullptr;
  } else {
    snprintf(barBuf, sizeof(barBuf), "%.2f", r.bar);
    snprintf(mpaBuf, sizeof(mpaBuf), "%.3f", r.mpa);
    doc["bar"] = serialized(barBuf);
    doc["mpa"] = serialized(mpaBuf);
  }

  snprintf(voltBuf, sizeof(voltBuf), "%.3f", r.volts);
  if (isfinite(r.volts)) doc["volt"] = serialized(voltBuf);
  else doc["volt"] = nullptr;

  doc["raw"]   = r.raw;
  doc["rssi"]  = rssi;
  doc["over"]  = r.over;
  doc["fault"] = r.fault;
  doc["heap"]  = heap;
  doc["raw_instant"] = r.rawInstant;
  doc["age_ms"] = r.ageMs;
  doc["stale"] = r.stale;

  if (doc.overflowed() || measureJson(doc) >= size) { if (size) out[0] = '\0'; return 0; }
  return serializeJson(doc, out, size);
}

size_t mqttDiscoveryJson(char *out, size_t size, const Settings &s, const char *deviceId,
                         const char *key, const char *name, const char *devClass,
                         const char *unit, const char *valueTemplate, bool diagnostic,
                         const char *configUrl) {
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

  // expire_after: без него Home Assistant показывает последнее полученное
  // значение как актуальное сколько угодно долго. Для датчика давления это
  // прямо вводит в заблуждение — пропавшее устройство выглядит исправным.
  // Три периода публикации: одна потерянная посылка не должна гасить сущность.
  doc["exp_aft"] = (uint32_t)s.mqttIntervalS * 3 + 10;

  // Общий блок device склеивает все сущности в одно устройство в интерфейсе HA.
  JsonObject dev = doc["dev"].to<JsonObject>();
  dev["ids"].to<JsonArray>().add(deviceId);
  dev["name"] = s.deviceName;
  dev["mdl"]  = "Wemos D1 mini";
  dev["mf"]   = "Espressif";
  dev["sw"]   = FW_VERSION;
  if (configUrl) dev["cu"] = configUrl;  // configuration_url: ссылка на портал

  if (doc.overflowed() || measureJson(doc) >= size) { if (size) out[0] = '\0'; return 0; }
  return serializeJson(doc, out, size);
}

// Часть Logic.cpp: проверяется и на устройстве, и в native-тестах.
static bool validTopic(const char *topic, bool allowEmpty) {
  if (!topic[0]) return allowEmpty;
  for (const unsigned char *p = (const unsigned char *)topic; *p; ++p)
    if (*p < 32 || *p == 127 || *p == '+' || *p == '#') return false;
  return topic[strlen(topic) - 1] != '/';
}

const char *settingsValidationError(const Settings &s) {
  if (s.mqttTlsMode > TLS_CA) return "Неизвестный режим TLS. Выберите режим проверки.";
  if (s.mqttTlsMode == TLS_FINGERPRINT) {
    uint8_t fp[TLS_FINGERPRINT_LEN];
    if (!parseFingerprint(s.mqttFingerprint, fp)) return "Отпечаток SHA-1 должен содержать 20 байт hex.";
  }
  if (!s.mqttPort) return "Порт MQTT: от 1 до 65535.";
  if (s.mqttIntervalS < 1 || s.mqttIntervalS > 3600) return "Период MQTT: от 1 до 3600 секунд.";
  if (!validTopic(s.mqttBase, false) || !validTopic(s.haPrefix, true))
    return "Топики не должны содержать +, #, управляющие символы или завершающий /.";
  if (!s.deviceName[0]) return "Укажите имя устройства.";
  if (!(s.adcFullScaleV >= 1.0f && s.adcFullScaleV <= 10.0f)) return "Шкала АЦП: от 1 до 10 В.";
  if (!isfinite(s.vZero) || !isfinite(s.vFull) || s.vZero < 0 || s.vFull > 10 ||
      !(s.vFull - s.vZero > 0.1f) || s.vZero >= s.adcFullScaleV)
    return "Калибровка: 0 ≤ ноль < шкала АЦП, размах > 0,1 В, максимум ≤ 10 В.";
  if (!(s.pFullMPa > 0.0f && s.pFullMPa <= 100.0f)) return "Диапазон давления: больше 0 и не более 100 МПа.";
  for (const char *p = s.mqttHost; *p; ++p)
    if ((unsigned char)*p <= 32 || *p == '/' || *p == ':') return "Адрес брокера: имя или IPv4, без протокола и порта.";
  return nullptr;
}

bool settingsPatch(JsonObjectConst patch, Settings &candidate, char *error, size_t errorSize) {
  Settings next = candidate;
  auto fail = [&](const char *message) { copyField(error, errorSize, message); return false; };
  if (patch.isNull()) return fail("Ожидается JSON-объект настроек.");
  struct TextField { const char *key; char *value; size_t size; };
  TextField texts[] = {
    {"mqttHost", next.mqttHost, sizeof(next.mqttHost)},
    {"mqttUser", next.mqttUser, sizeof(next.mqttUser)},
    {"mqttPass", next.mqttPass, sizeof(next.mqttPass)},
    {"mqttBase", next.mqttBase, sizeof(next.mqttBase)},
    {"haPrefix", next.haPrefix, sizeof(next.haPrefix)},
    {"deviceName", next.deviceName, sizeof(next.deviceName)},
    {"mqttFingerprint", next.mqttFingerprint, sizeof(next.mqttFingerprint)},
    {"ntpServer", next.ntpServer, sizeof(next.ntpServer)},
    {"portalPass", next.portalPass, sizeof(next.portalPass)}
  };
  for (const auto &pair : patch) {
    const char *key = pair.key().c_str();
    bool known = false;
    for (auto &field : texts) if (!strcmp(key, field.key)) {
      known = true;
      if (!pair.value().is<const char *>()) return fail("Текстовое поле имеет неверный тип.");
      const char *value = pair.value().as<const char *>();
      if (strlen(value) >= field.size) return fail("Слишком длинное значение (ограничение в байтах UTF-8).");
      if (!strcmp(key, "portalPass")) {
        if (strlen(value) < 12) return fail("Пароль интерфейса: от 12 до 63 символов ASCII.");
        for (const char *p = value; *p; ++p)
          if ((unsigned char)*p < 33 || (unsigned char)*p > 126) return fail("Пароль: печатные символы ASCII без пробелов.");
      }
      copyField(field.value, field.size, value);
    }
    if (known) continue;
    if (!strcmp(key, "mqttPort") || !strcmp(key, "mqttIntervalS") || !strcmp(key, "mqttTlsMode")) {
      if (!pair.value().is<unsigned int>()) return fail("Ожидается целое неотрицательное число.");
      unsigned int value = pair.value().as<unsigned int>();
      if (!strcmp(key, "mqttPort")) { if (value < 1 || value > 65535) return fail("Порт MQTT: от 1 до 65535."); next.mqttPort = value; }
      if (!strcmp(key, "mqttIntervalS")) { if (value < 1 || value > 3600) return fail("Период MQTT: от 1 до 3600 секунд."); next.mqttIntervalS = value; }
      if (!strcmp(key, "mqttTlsMode")) { if (value > TLS_CA) return fail("Неизвестный режим TLS."); next.mqttTlsMode = value; }
    } else if (!strcmp(key, "adcFullScaleV") || !strcmp(key, "vZero") ||
               !strcmp(key, "vFull") || !strcmp(key, "pFullMPa")) {
      if (!pair.value().is<float>()) return fail("Калибровка должна содержать числа.");
      float value = pair.value().as<float>();
      if (!isfinite(value)) return fail("Калибровка должна содержать конечные числа.");
      if (!strcmp(key, "adcFullScaleV")) next.adcFullScaleV = value;
      if (!strcmp(key, "vZero")) next.vZero = value;
      if (!strcmp(key, "vFull")) next.vFull = value;
      if (!strcmp(key, "pFullMPa")) next.pFullMPa = value;
    } else return fail("Неизвестное поле настроек.");
  }
  if (const char *message = settingsValidationError(next)) return fail(message);
  candidate = next;
  if (errorSize) error[0] = '\0';
  return true;
}

void pressureFilterSample(PressureFilter &f, int raw, uint32_t now, const Settings &s) {
  if (!f.ready || uint32_t(now - f.sampledMs) >= SENSOR_FILTER_RESET_MS) {
    if (f.over) f.overHoldMs = now;
    if (f.fault) f.faultHoldMs = now;
    for (int &value : f.samples) value = raw;
    f.total = raw * int32_t(PRESSURE_FILTER_SAMPLES);
    f.index = 0;
  } else {
    f.total -= f.samples[f.index];
    f.samples[f.index] = raw;
    f.total += raw;
    f.index = (f.index + 1) % PRESSURE_FILTER_SAMPLES;
  }
  f.latest = raw;
  f.sampledMs = now;
  f.ready = true;
  Reading instant = pressureConvert(raw, s);
  if (instant.over) f.over = true;
  if (instant.fault) f.fault = true;
  // Немедленно обнаруживаем нарушение; восстанавливаемся после секунды устойчивого сигнала.
  if (instant.over || raw > SENSOR_RAW_OVER - 10 || instant.volts > s.vFull) f.overHoldMs = now;
  else if (uint32_t(now - f.overHoldMs) >= SENSOR_RECOVERY_MS) f.over = false;
  if (instant.fault || instant.volts < s.vZero - SENSOR_FAULT_MARGIN_V / 2) f.faultHoldMs = now;
  else if (uint32_t(now - f.faultHoldMs) >= SENSOR_RECOVERY_MS) f.fault = false;
}

Reading pressureFilterRead(const PressureFilter &f, uint32_t now, const Settings &s) {
  Reading r = pressureConvert(f.ready ? f.total / int32_t(PRESSURE_FILTER_SAMPLES) : 0, s);
  r.rawInstant = f.latest;
  r.ageMs = uint32_t(now - f.sampledMs);
  r.stale = !f.ready || r.ageMs >= SENSOR_STALE_MS;
  r.over = r.over || f.over;
  r.fault = r.fault || f.fault;
  return r;
}

bool mqttReconcileDiscovery(Settings &s, DiscoveryWriter publish, SettingsWriter persist, void *context) {
  const bool changed = strcmp(s.haPrefixApplied, s.haPrefix) != 0;
  if (changed && s.haPrefixApplied[0]) {
    for (size_t i = 0; i < MQTT_ENTITY_COUNT; ++i)
      if (!publish(s.haPrefixApplied, MQTT_ENTITY_KEYS[i], true, context)) return false;
  }
  if (s.haPrefix[0]) {
    for (size_t i = 0; i < MQTT_ENTITY_COUNT; ++i)
      if (!publish(s.haPrefix, MQTT_ENTITY_KEYS[i], false, context)) return false;
  }
  if (changed) {
    Settings candidate = s;
    copyField(candidate.haPrefixApplied, sizeof(candidate.haPrefixApplied), s.haPrefix);
    if (!persist(candidate, context)) return false;
    s = candidate;
  }
  return true;
}
