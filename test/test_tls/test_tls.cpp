// Тесты разбора отпечатка сертификата и валидации настроек TLS.
//
// parseFingerprint() — единственное место, где строка из формы портала
// превращается в то, чем BearSSL сверяет сертификат. Ошибка здесь не падает и
// не логируется: устройство просто перестаёт подключаться к брокеру, а причина
// выглядит как «TLS не работает».

#include <unity.h>

#include <string.h>

#include "Logic.h"

static const char *DEV = "wps_abc123";

// Один и тот же отпечаток в разных написаниях.
static const char *FP_COLONS = "A1:B2:C3:D4:E5:F6:07:18:29:3A:4B:5C:6D:7E:8F:90:A1:B2:C3:D4";
static const char *FP_PLAIN  = "A1B2C3D4E5F60718293A4B5C6D7E8F90A1B2C3D4";
static const char *FP_LOWER  = "a1:b2:c3:d4:e5:f6:07:18:29:3a:4b:5c:6d:7e:8f:90:a1:b2:c3:d4";

static const uint8_t FP_BYTES[TLS_FINGERPRINT_LEN] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07,
                                                      0x18, 0x29, 0x3A, 0x4B, 0x5C, 0x6D, 0x7E,
                                                      0x8F, 0x90, 0xA1, 0xB2, 0xC3, 0xD4};

static void makeDefaults(Settings &s) {
  memset(&s, 0, sizeof(s));
  settingsDefaults(s, DEV);
}

static void setStr(char *dst, size_t size, const char *v) {
  memset(dst, 0, size);
  strncpy(dst, v, size - 1);
}

void setUp(void) {}
void tearDown(void) {}

// ------------------------------------------------------- разбор отпечатка ---

static void test_fingerprint_with_colons(void) {
  uint8_t fp[TLS_FINGERPRINT_LEN];
  TEST_ASSERT_TRUE(parseFingerprint(FP_COLONS, fp));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(FP_BYTES, fp, TLS_FINGERPRINT_LEN);
}

static void test_fingerprint_without_separators(void) {
  // openssl x509 -fingerprint выдаёт с двоеточиями, но вставляют и голый hex.
  uint8_t fp[TLS_FINGERPRINT_LEN];
  TEST_ASSERT_TRUE(parseFingerprint(FP_PLAIN, fp));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(FP_BYTES, fp, TLS_FINGERPRINT_LEN);
}

static void test_fingerprint_is_case_insensitive(void) {
  uint8_t fp[TLS_FINGERPRINT_LEN];
  TEST_ASSERT_TRUE(parseFingerprint(FP_LOWER, fp));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(FP_BYTES, fp, TLS_FINGERPRINT_LEN);
}

static void test_fingerprint_rejects_empty(void) {
  uint8_t fp[TLS_FINGERPRINT_LEN];
  TEST_ASSERT_FALSE(parseFingerprint("", fp));
}

static void test_fingerprint_rejects_null(void) {
  uint8_t fp[TLS_FINGERPRINT_LEN];
  TEST_ASSERT_FALSE(parseFingerprint(nullptr, fp));
}

static void test_fingerprint_rejects_too_short(void) {
  // 19 байт вместо 20. Дополнить нулями было бы хуже всего: получился бы
  // синтаксически корректный отпечаток, не совпадающий ни с одним сертификатом.
  uint8_t fp[TLS_FINGERPRINT_LEN];
  TEST_ASSERT_FALSE(parseFingerprint("A1:B2:C3:D4:E5:F6:07:18:29:3A:4B:5C:6D:7E:8F:90:A1:B2:C3", fp));
  TEST_ASSERT_FALSE(parseFingerprint("A1", fp));
  TEST_ASSERT_FALSE(parseFingerprint("A", fp));
}

static void test_fingerprint_rejects_too_long(void) {
  uint8_t fp[TLS_FINGERPRINT_LEN];
  TEST_ASSERT_FALSE(parseFingerprint("A1B2C3D4E5F60718293A4B5C6D7E8F90A1B2C3D4EE", fp));
  TEST_ASSERT_FALSE(parseFingerprint("A1:B2:C3:D4:E5:F6:07:18:29:3A:4B:5C:6D:7E:8F:90:A1:B2:C3:D4:EE",
                                     fp));
}

static void test_fingerprint_rejects_trailing_separator(void) {
  uint8_t fp[TLS_FINGERPRINT_LEN];
  TEST_ASSERT_FALSE(parseFingerprint(
      "A1:B2:C3:D4:E5:F6:07:18:29:3A:4B:5C:6D:7E:8F:90:A1:B2:C3:D4:", fp));
}

static void test_fingerprint_rejects_bad_characters(void) {
  uint8_t fp[TLS_FINGERPRINT_LEN];
  // 'G' не hex; пробелы как разделитель не поддерживаем; двойное двоеточие.
  TEST_ASSERT_FALSE(parseFingerprint("G1B2C3D4E5F60718293A4B5C6D7E8F90A1B2C3D4", fp));
  TEST_ASSERT_FALSE(parseFingerprint("A1 B2 C3 D4 E5 F6 07 18 29 3A 4B 5C 6D 7E 8F 90 A1 B2 C3 D4",
                                     fp));
  TEST_ASSERT_FALSE(parseFingerprint("A1::B2:C3:D4:E5:F6:07:18:29:3A:4B:5C:6D:7E:8F:90:A1:B2:C3:D4",
                                     fp));
  TEST_ASSERT_FALSE(parseFingerprint(":A1:B2:C3:D4:E5:F6:07:18:29:3A:4B:5C:6D:7E:8F:90:A1:B2:C3:D4",
                                     fp));
}

static void test_fingerprint_leaves_output_untouched_on_error(void) {
  // Вызывающий может передать буфер с прежним отпечатком: испортить его при
  // ошибке разбора значило бы сломать работающее соединение.
  uint8_t fp[TLS_FINGERPRINT_LEN];
  memset(fp, 0xEE, sizeof(fp));

  TEST_ASSERT_FALSE(parseFingerprint("не отпечаток", fp));
  for (size_t i = 0; i < TLS_FINGERPRINT_LEN; i++) TEST_ASSERT_EQUAL_UINT8(0xEE, fp[i]);
}

// -------------------------------------------------------- валидация TLS ---

static void test_defaults_disable_tls(void) {
  // Свежее устройство не должно молча пытаться в TLS: настройка осознанная.
  Settings s;
  makeDefaults(s);
  TEST_ASSERT_EQUAL_UINT8(TLS_OFF, s.mqttTlsMode);
  TEST_ASSERT_EQUAL_STRING("", s.mqttFingerprint);
  TEST_ASSERT_EQUAL_STRING("", s.ntpServer);
}

static void test_unknown_tls_mode_blocks_connection(void) {
  Settings s;
  makeDefaults(s);
  s.mqttTlsMode = 7;
  TEST_ASSERT_FALSE(settingsSanitize(s, DEV));
  TEST_ASSERT_EQUAL_UINT8(7, s.mqttTlsMode);
  TEST_ASSERT_NOT_NULL(settingsValidationError(s));
}

static void test_valid_tls_modes_accepted(void) {
  const uint8_t modes[] = {TLS_OFF, TLS_INSECURE, TLS_CA};
  for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
    Settings s;
    makeDefaults(s);
    s.mqttTlsMode = modes[i];
    TEST_ASSERT_FALSE(settingsSanitize(s, DEV));
    TEST_ASSERT_EQUAL_UINT8(modes[i], s.mqttTlsMode);
  }
}

static void test_fingerprint_mode_with_valid_fingerprint_accepted(void) {
  Settings s;
  makeDefaults(s);
  s.mqttTlsMode = TLS_FINGERPRINT;
  setStr(s.mqttFingerprint, sizeof(s.mqttFingerprint), FP_COLONS);

  TEST_ASSERT_FALSE(settingsSanitize(s, DEV));
  TEST_ASSERT_EQUAL_UINT8(TLS_FINGERPRINT, s.mqttTlsMode);
}

static void test_broken_fingerprint_blocks_connection(void) {
  // Опечатка не должна незаметно отключать проверку подлинности брокера.
  Settings s;
  makeDefaults(s);
  s.mqttTlsMode = TLS_FINGERPRINT;
  setStr(s.mqttFingerprint, sizeof(s.mqttFingerprint), "мусор");

  TEST_ASSERT_FALSE(settingsSanitize(s, DEV));
  TEST_ASSERT_EQUAL_UINT8(TLS_FINGERPRINT, s.mqttTlsMode);
  TEST_ASSERT_NOT_NULL(settingsValidationError(s));
}

static void test_empty_fingerprint_blocks_connection(void) {
  Settings s;
  makeDefaults(s);
  s.mqttTlsMode = TLS_FINGERPRINT;

  TEST_ASSERT_FALSE(settingsSanitize(s, DEV));
  TEST_ASSERT_EQUAL_UINT8(TLS_FINGERPRINT, s.mqttTlsMode);
  TEST_ASSERT_NOT_NULL(settingsValidationError(s));
}

// -------------------------------------------------------- порт по режиму ---

static void test_zero_port_without_tls_becomes_1883(void) {
  Settings s;
  makeDefaults(s);
  s.mqttPort = 0;
  TEST_ASSERT_TRUE(settingsSanitize(s, DEV));
  TEST_ASSERT_EQUAL_UINT16(1883, s.mqttPort);
}

static void test_zero_port_with_tls_becomes_8883(void) {
  Settings s;
  makeDefaults(s);
  s.mqttPort    = 0;
  s.mqttTlsMode = TLS_CA;
  TEST_ASSERT_TRUE(settingsSanitize(s, DEV));
  TEST_ASSERT_EQUAL_UINT16(8883, s.mqttPort);
}

static void test_explicit_port_is_not_touched_by_tls(void) {
  // TLS на нестандартном порту — законная конфигурация, подставлять 8883
  // поверх заданного числа нельзя.
  Settings s;
  makeDefaults(s);
  s.mqttPort    = 1883;
  s.mqttTlsMode = TLS_INSECURE;
  TEST_ASSERT_FALSE(settingsSanitize(s, DEV));
  TEST_ASSERT_EQUAL_UINT16(1883, s.mqttPort);
}

static void test_sanitize_is_idempotent_with_tls(void) {
  Settings s;
  makeDefaults(s);
  s.mqttTlsMode = 9;
  s.mqttPort    = 0;

  TEST_ASSERT_TRUE(settingsSanitize(s, DEV));

  Settings after = s;
  TEST_ASSERT_FALSE(settingsSanitize(s, DEV));
  TEST_ASSERT_EQUAL_MEMORY(&after, &s, sizeof(Settings));
}

int main(int, char **) {
  UNITY_BEGIN();

  RUN_TEST(test_fingerprint_with_colons);
  RUN_TEST(test_fingerprint_without_separators);
  RUN_TEST(test_fingerprint_is_case_insensitive);
  RUN_TEST(test_fingerprint_rejects_empty);
  RUN_TEST(test_fingerprint_rejects_null);
  RUN_TEST(test_fingerprint_rejects_too_short);
  RUN_TEST(test_fingerprint_rejects_too_long);
  RUN_TEST(test_fingerprint_rejects_trailing_separator);
  RUN_TEST(test_fingerprint_rejects_bad_characters);
  RUN_TEST(test_fingerprint_leaves_output_untouched_on_error);

  RUN_TEST(test_defaults_disable_tls);
  RUN_TEST(test_unknown_tls_mode_blocks_connection);
  RUN_TEST(test_valid_tls_modes_accepted);
  RUN_TEST(test_fingerprint_mode_with_valid_fingerprint_accepted);
  RUN_TEST(test_broken_fingerprint_blocks_connection);
  RUN_TEST(test_empty_fingerprint_blocks_connection);

  RUN_TEST(test_zero_port_without_tls_becomes_1883);
  RUN_TEST(test_zero_port_with_tls_becomes_8883);
  RUN_TEST(test_explicit_port_is_not_touched_by_tls);
  RUN_TEST(test_sanitize_is_idempotent_with_tls);

  return UNITY_END();
}

