#pragma once

#include <ArduinoJson.h>

#include <stddef.h>
#include <stdint.h>

// Чистая логика прошивки: арифметика датчика, валидация настроек, их
// сериализация и сборка MQTT-топиков и payload. Здесь намеренно нет ни одного
// обращения к железу и ни одного включения Arduino.h — благодаря этому модуль
// собирается и тестируется на хосте (см. test/ и окружение [env:native]).

// ---------------------------------------------------------------- настройки ---

// Как проверять сертификат брокера. Влияет и на требования к памяти: без
// проверки BearSSL всё равно нужен приёмный буфер под запись TLS, а вот часы
// нужны только режиму TLS_CA — проверка срока действия сертификата без них
// гарантированно провалится.
enum TlsMode : uint8_t {
  TLS_OFF         = 0,  // обычный TCP, ни шифрования, ни проверки
  TLS_INSECURE    = 1,  // шифрование без проверки: защищает от прослушивания, не от MITM
  TLS_FINGERPRINT = 2,  // пиннинг по SHA-1 сертификата, часы не нужны
  TLS_CA          = 3,  // проверка по загруженному CA, нужно точное время
};

// Отпечаток в виде строки: 20 байт hex через двоеточие = 59 символов.
#define TLS_FINGERPRINT_LEN 20

struct Settings {
  // --- MQTT ---
  char     mqttHost[64];   // пусто => MQTT выключен, устройство работает автономно
  uint16_t mqttPort;
  char     mqttUser[32];   // пусто => без аутентификации
  char     mqttPass[64];
  char     mqttBase[64];   // базовый топик
  uint16_t mqttIntervalS;  // период публикации состояния
  char     haPrefix[32];   // пусто => не публиковать discovery Home Assistant
  char     deviceName[32]; // имя устройства в Home Assistant

  // Префикс, под которым discovery реально опубликован. Отличается от haPrefix
  // ровно до тех пор, пока старые retained-топики не погашены: без этого смена
  // префикса оставляет в Home Assistant устройство-призрак.
  char haPrefixApplied[32];
  char portalPass[64]; // пароль настройки; не возвращается браузеру

  // --- TLS ---
  uint8_t mqttTlsMode;          // TlsMode
  char    mqttFingerprint[64];  // "AA:BB:..:TT" для TLS_FINGERPRINT
  char    ntpServer[64];        // для TLS_CA; пусто => NTP_SERVER_DEFAULT

  // --- калибровка ---
  float adcFullScaleV;  // напряжение датчика при 1023 с учётом внешнего делителя
  float vZero;          // выход датчика при нулевом давлении, В
  float vFull;          // выход датчика при полной шкале, В
  float pFullMPa;       // полная шкала датчика, МПа
};

// NTP-сервер по умолчанию. Поле настроек нужно для сетей без интернета, где
// время раздаёт роутер: без часов режим TLS_CA не работает.
#define NTP_SERVER_DEFAULT "pool.ntp.org"

void settingsDefaults(Settings &s, const char *deviceId);

// Разбирает отпечаток сертификата: "AA:BB:..:TT" или сплошные 40 hex-символов,
// регистр любой. false, если это не ровно TLS_FINGERPRINT_LEN байт — тогда
// out не трогается. Разделителем считается только ':', и он обязан стоять
// строго между парами.
bool parseFingerprint(const char *s, uint8_t out[TLS_FINGERPRINT_LEN]);

// Приводит настройки к валидному виду, возвращает true, если что-то поправлено.
// Главное, что здесь гарантируется: vFull - vZero > 0.1, поэтому pressureConvert()
// не может разделить на ноль. haPrefix намеренно не проверяется — пустая строка
// там осмысленна и означает «не публиковать discovery».
bool settingsSanitize(Settings &s, const char *deviceId);
const char *settingsValidationError(const Settings &s);
bool settingsPatch(JsonObjectConst patch, Settings &candidate, char *error, size_t errorSize);
using DiscoveryWriter = bool (*)(const char *prefix, const char *key, bool remove, void *context);
using SettingsWriter = bool (*)(const Settings &settings, void *context);
bool mqttReconcileDiscovery(Settings &s, DiscoveryWriter publish, SettingsWriter persist, void *context);

// Сериализация настроек. Живёт здесь, а не в Settings.cpp (LittleFS), ровно
// затем, чтобы её покрывал round-trip тест: раньше список полей был продублирован
// в чтении и в записи, и забытое в записи поле не пережило бы перезагрузку —
// молча и без единого падающего теста.
void settingsToJson(JsonObject dst, const Settings &s);

// Отсутствующие и нечитаемые ключи не трогает: значение остаётся тем, что было
// в s. Поэтому config.json от прежней версии прошивки читается без потерь.
void settingsFromJson(JsonObjectConst src, Settings &s);

// ----------------------------------------------------------------- измерение ---

// Результат одного пересчёта показаний датчика давления.
struct Reading {
  int   raw;    // усреднённый отсчёт АЦП, 0..1023
  float volts;  // восстановленное напряжение датчика, до внешнего делителя
  float mpa;    // давление, МПа
  float bar;    // давление, бар

  // АЦП упёрся в потолок: давление выше измеримого диапазона и неизвестно.
  bool over;

  // Напряжение ниже физического минимума датчика — обрыв или датчик не подключён.
  bool fault;
  int rawInstant = 0;
  uint32_t ageMs = 0;
  bool stale = false;
};

// Отсчёт, начиная с которого считаем, что АЦП упёрся в потолок.
constexpr int SENSOR_RAW_OVER = 1020;

// Насколько ниже vZero должно упасть напряжение, чтобы счесть датчик оборванным.
// При отключённом A0 плата читает около нуля.
constexpr float SENSOR_FAULT_MARGIN_V = 0.2f;
constexpr size_t PRESSURE_FILTER_SAMPLES = 32;
constexpr uint32_t SENSOR_STALE_MS = 1000;
constexpr uint32_t SENSOR_RECOVERY_MS = 1000;
constexpr uint32_t SENSOR_FILTER_RESET_MS = 500;

struct PressureFilter {
  int samples[PRESSURE_FILTER_SAMPLES] = {};
  int32_t total = 0;
  size_t index = 0;
  int latest = 0;
  uint32_t sampledMs = 0;
  uint32_t overHoldMs = 0;
  uint32_t faultHoldMs = 0;
  bool ready = false;
  bool over = false;
  bool fault = false;
};
void pressureFilterSample(PressureFilter &filter, int raw, uint32_t now, const Settings &s);
Reading pressureFilterRead(const PressureFilter &filter, uint32_t now, const Settings &s);

Reading pressureConvert(int raw, const Settings &s);

// Верхняя граница измеримого давления при текущей калибровке, бар.
float pressureMaxBar(const Settings &s);

// ---------------------------------------------------------------------- MQTT ---

// Собирает "<base>/<suffix>".
void mqttTopic(char *out, size_t size, const Settings &s, const char *suffix);

// Собирает "<haPrefix>/sensor/<deviceId>/<key>/config".
void mqttDiscoveryTopic(char *out, size_t size, const Settings &s, const char *deviceId,
                        const char *key);

// Payload состояния. Возвращает число записанных байт (без завершающего нуля).
size_t mqttStateJson(char *out, size_t size, const Reading &r, int rssi, uint32_t heap);

// Собирает discovery-топик для произвольного префикса. Нужен, чтобы погасить
// retained-конфигурацию под старым префиксом: иначе в Home Assistant остаётся
// устройство-призрак, которое убирается только руками через mosquitto_pub.
void mqttDiscoveryTopicFor(char *out, size_t size, const char *haPrefix, const char *deviceId,
                           const char *key);

// Ключи всех сущностей, публикуемых в discovery. Один список на публикацию и на
// уборку: разойдясь, они оставили бы часть сущностей непогашенной.
extern const char *const MQTT_ENTITY_KEYS[];
extern const size_t      MQTT_ENTITY_COUNT;

// Payload discovery для одной сущности Home Assistant.
// devClass и unit могут быть nullptr — тогда соответствующие ключи опускаются.
// configUrl — адрес портала устройства; nullptr, если он ещё неизвестен.
size_t mqttDiscoveryJson(char *out, size_t size, const Settings &s, const char *deviceId,
                         const char *key, const char *name, const char *devClass,
                         const char *unit, const char *valueTemplate, bool diagnostic,
                         const char *configUrl);
