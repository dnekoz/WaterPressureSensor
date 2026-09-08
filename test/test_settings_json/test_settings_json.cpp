// Тесты сериализации настроек.
//
// Главный тест здесь — test_roundtrip_preserves_every_field. Список полей
// Settings продублирован в settingsToJson() и settingsFromJson(), и поле,
// забытое в записи, не пережило бы перезагрузку устройства: настройка молча
// возвращалась бы к значению по умолчанию. Round-trip ловит это на хосте.

#include <unity.h>

#include <math.h>
#include <string.h>

#include "Logic.h"

static const char *DEV = "wps_abc123";

// Заполняется на месте и всегда с memset: сравнение идёт по всей структуре, а в
// ней есть байты выравнивания. Без обнуления они содержали бы мусор со стека, и
// TEST_ASSERT_EQUAL_MEMORY падал бы через раз безо всякой связи с логикой.
static void makeDefaults(Settings &s) {
  memset(&s, 0, sizeof(s));
  settingsDefaults(s, DEV);
}

// Затирает поле целиком, а не только до завершающего нуля: strcpy() оставил бы
// за нулём хвост прежней строки, а settingsFromJson() пишет через strncpy(),
// который хвост обнуляет.
static void setStr(char *dst, size_t size, const char *v) {
  memset(dst, 0, size);
  strncpy(dst, v, size - 1);
}

// Настройки, у которых каждое поле отличается от значения по умолчанию: иначе
// round-trip прошёл бы и с полем, которое вообще не сериализуется.
static void makeAllFieldsChanged(Settings &s) {
  makeDefaults(s);

  setStr(s.mqttHost, sizeof(s.mqttHost), "broker.example.org");
  s.mqttPort = 8883;
  setStr(s.mqttUser, sizeof(s.mqttUser), "sensor");
  setStr(s.mqttPass, sizeof(s.mqttPass), "s3cret");
  setStr(s.mqttBase, sizeof(s.mqttBase), "home/boiler/pressure");
  s.mqttIntervalS = 42;
  setStr(s.haPrefix, sizeof(s.haPrefix), "ha");
  setStr(s.haPrefixApplied, sizeof(s.haPrefixApplied), "homeassistant");
  setStr(s.deviceName, sizeof(s.deviceName), "Boiler loop");

  s.mqttTlsMode = TLS_FINGERPRINT;
  setStr(s.mqttFingerprint, sizeof(s.mqttFingerprint),
         "A1:B2:C3:D4:E5:F6:07:18:29:3A:4B:5C:6D:7E:8F:90:A1:B2:C3:D4");
  setStr(s.ntpServer, sizeof(s.ntpServer), "192.168.1.1");

  s.adcFullScaleV = 4.75f;
  s.vZero         = 0.42f;
  s.vFull         = 4.42f;
  s.pFullMPa      = 1.2f;
}

// Прогоняет настройки через JSON и обратно поверх значений по умолчанию.
static void roundtrip(const Settings &src, Settings &dst) {
  JsonDocument doc;
  settingsToJson(doc.to<JsonObject>(), src);

  char   buf[1024];
  size_t len = serializeJson(doc, buf, sizeof(buf));
  TEST_ASSERT_TRUE_MESSAGE(len > 0 && len < sizeof(buf), "payload настроек не поместился");

  JsonDocument parsed;
  TEST_ASSERT_FALSE(deserializeJson(parsed, buf));

  makeDefaults(dst);
  settingsFromJson(parsed.as<JsonObjectConst>(), dst);
}

void setUp(void) {}
void tearDown(void) {}

// ------------------------------------------------------------- round-trip ---

static void test_roundtrip_preserves_every_field(void) {
  // Сравнение всей структуры целиком, а не поле за полем: тест не придётся
  // дополнять при добавлении новых настроек, он сломается сам.
  Settings src, dst;
  makeAllFieldsChanged(src);
  roundtrip(src, dst);
  TEST_ASSERT_EQUAL_MEMORY(&src, &dst, sizeof(Settings));
}

static void test_roundtrip_of_defaults(void) {
  Settings src, dst;
  makeDefaults(src);
  roundtrip(src, dst);
  TEST_ASSERT_EQUAL_MEMORY(&src, &dst, sizeof(Settings));
}

static void test_roundtrip_preserves_empty_strings(void) {
  // Пустые mqttHost и haPrefix — не «не задано», а осмысленные значения:
  // выключенный MQTT и выключенный discovery.
  Settings src, dst;
  makeDefaults(src);
  setStr(src.mqttHost, sizeof(src.mqttHost), "");
  setStr(src.haPrefix, sizeof(src.haPrefix), "");

  roundtrip(src, dst);
  TEST_ASSERT_EQUAL_STRING("", dst.mqttHost);
  TEST_ASSERT_EQUAL_STRING("", dst.haPrefix);
  TEST_ASSERT_EQUAL_MEMORY(&src, &dst, sizeof(Settings));
}

// ------------------------------------------------- совместимость и мусор ---

static void test_missing_keys_keep_current_values(void) {
  // config.json от прежней версии прошивки: ключей TLS в нём ещё нет.
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, "{\"mqttHost\":\"10.0.0.5\"}"));

  Settings s;
  makeDefaults(s);
  settingsFromJson(doc.as<JsonObjectConst>(), s);

  TEST_ASSERT_EQUAL_STRING("10.0.0.5", s.mqttHost);
  TEST_ASSERT_EQUAL_UINT16(1883, s.mqttPort);
  TEST_ASSERT_EQUAL_STRING("homeassistant", s.haPrefix);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.2f, s.adcFullScaleV);
  TEST_ASSERT_EQUAL_UINT8(TLS_OFF, s.mqttTlsMode);
}

static void test_empty_object_changes_nothing(void) {
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, "{}"));

  Settings s, expected;
  makeDefaults(s);
  makeDefaults(expected);
  settingsFromJson(doc.as<JsonObjectConst>(), s);

  TEST_ASSERT_EQUAL_MEMORY(&expected, &s, sizeof(Settings));
}

static void test_wrong_types_do_not_corrupt_settings(void) {
  // Число вместо строки и строка вместо числа: разбор не должен ни падать,
  // ни писать мусор в поля.
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(
      doc, "{\"mqttHost\":42,\"mqttPort\":\"нет\",\"vFull\":\"abc\",\"deviceName\":null,"
           "\"mqttFingerprint\":[1,2,3]}"));

  Settings s;
  makeDefaults(s);
  settingsFromJson(doc.as<JsonObjectConst>(), s);

  TEST_ASSERT_EQUAL_STRING("", s.mqttHost);
  TEST_ASSERT_EQUAL_UINT16(1883, s.mqttPort);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.5f, s.vFull);
  TEST_ASSERT_EQUAL_STRING("Water Pressure", s.deviceName);
  TEST_ASSERT_EQUAL_STRING("", s.mqttFingerprint);
}

static void test_overlong_strings_are_truncated_safely(void) {
  // Поля Settings фиксированной длины: слишком длинное значение должно
  // обрезаться с завершающим нулём, а не переехать в соседнее поле.
  char json[400];
  snprintf(json, sizeof(json),
           "{\"mqttHost\":\"%s\",\"deviceName\":\"%s\"}",
           "hhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhh",
           "nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn");

  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, json));

  Settings s;
  makeDefaults(s);
  settingsFromJson(doc.as<JsonObjectConst>(), s);

  TEST_ASSERT_EQUAL_size_t(sizeof(s.mqttHost) - 1, strlen(s.mqttHost));
  TEST_ASSERT_EQUAL_size_t(sizeof(s.deviceName) - 1, strlen(s.deviceName));
  TEST_ASSERT_EQUAL_UINT16(1883, s.mqttPort);  // соседнее поле не задето
}

static void test_roundtrip_survives_sanitize(void) {
  // Полный путь загрузки: JSON -> настройки -> валидация. Повторная валидация
  // ничего менять не должна, иначе настройки «дрожали» бы при каждом старте.
  Settings src, dst;
  makeAllFieldsChanged(src);
  roundtrip(src, dst);

  TEST_ASSERT_FALSE(settingsSanitize(dst, DEV));
  TEST_ASSERT_EQUAL_MEMORY(&src, &dst, sizeof(Settings));
}

int main(int, char **) {
  UNITY_BEGIN();

  RUN_TEST(test_roundtrip_preserves_every_field);
  RUN_TEST(test_roundtrip_of_defaults);
  RUN_TEST(test_roundtrip_preserves_empty_strings);

  RUN_TEST(test_missing_keys_keep_current_values);
  RUN_TEST(test_empty_object_changes_nothing);
  RUN_TEST(test_wrong_types_do_not_corrupt_settings);
  RUN_TEST(test_overlong_strings_are_truncated_safely);
  RUN_TEST(test_roundtrip_survives_sanitize);

  return UNITY_END();
}
