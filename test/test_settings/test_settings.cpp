// Тесты значений по умолчанию и валидации настроек.
//
// Главное, что здесь проверяется: после settingsSanitize() знаменатель
// (vFull - vZero) заведомо не ноль, иначе pressureConvert() делил бы на ноль
// в рабочем цикле.

#include <unity.h>

#include <math.h>
#include <string.h>

#include "Logic.h"

static const char *DEV = "wps_abc123";

static Settings defaults() {
  Settings s;
  settingsDefaults(s, DEV);
  return s;
}

void setUp(void) {}
void tearDown(void) {}

// ------------------------------------------------------- значения по умолчанию ---

static void test_defaults_disable_mqtt(void) {
  // Пустой адрес брокера = MQTT выключен: свежее устройство не должно
  // никуда стучаться, пока его не настроили.
  TEST_ASSERT_EQUAL_STRING("", defaults().mqttHost);
}

static void test_defaults_base_topic_contains_device_id(void) {
  TEST_ASSERT_EQUAL_STRING("watersensor/wps_abc123", defaults().mqttBase);
}

static void test_defaults_match_xidibei_and_d1_mini(void) {
  Settings s = defaults();
  TEST_ASSERT_EQUAL_UINT16(1883, s.mqttPort);
  TEST_ASSERT_EQUAL_UINT16(10, s.mqttIntervalS);
  TEST_ASSERT_EQUAL_STRING("homeassistant", s.haPrefix);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.2f, s.adcFullScaleV);  // делитель D1 mini
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, s.vZero);          // XIDIBEI
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.5f, s.vFull);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, s.pFullMPa);
}

static void test_defaults_are_already_valid(void) {
  // Значения по умолчанию не должны нуждаться в правке.
  Settings s = defaults();
  TEST_ASSERT_FALSE(settingsSanitize(s, DEV));
}

static void test_defaults_handle_null_device_id(void) {
  Settings s;
  settingsDefaults(s, nullptr);
  TEST_ASSERT_EQUAL_STRING("watersensor/device", s.mqttBase);
}

// ------------------------------------------------------------------- MQTT ---

static void test_zero_port_reset(void) {
  Settings s = defaults();
  s.mqttPort = 0;
  TEST_ASSERT_TRUE(settingsSanitize(s, DEV));
  TEST_ASSERT_EQUAL_UINT16(1883, s.mqttPort);
}

static void test_zero_interval_reset(void) {
  Settings s      = defaults();
  s.mqttIntervalS = 0;
  TEST_ASSERT_TRUE(settingsSanitize(s, DEV));
  TEST_ASSERT_EQUAL_UINT16(10, s.mqttIntervalS);
}

static void test_huge_interval_reset(void) {
  Settings s      = defaults();
  s.mqttIntervalS = 3601;
  TEST_ASSERT_TRUE(settingsSanitize(s, DEV));
  TEST_ASSERT_EQUAL_UINT16(10, s.mqttIntervalS);
}

static void test_interval_bounds_are_inclusive(void) {
  Settings s      = defaults();
  s.mqttIntervalS = 1;
  TEST_ASSERT_FALSE(settingsSanitize(s, DEV));
  TEST_ASSERT_EQUAL_UINT16(1, s.mqttIntervalS);

  s.mqttIntervalS = 3600;
  TEST_ASSERT_FALSE(settingsSanitize(s, DEV));
  TEST_ASSERT_EQUAL_UINT16(3600, s.mqttIntervalS);
}

static void test_empty_base_topic_restored(void) {
  Settings s = defaults();
  s.mqttBase[0] = '\0';
  TEST_ASSERT_TRUE(settingsSanitize(s, DEV));
  TEST_ASSERT_EQUAL_STRING("watersensor/wps_abc123", s.mqttBase);
}

static void test_trailing_slash_stripped(void) {
  // Иначе топик получился бы "home/sensor//state".
  Settings s = defaults();
  strcpy(s.mqttBase, "home/sensor/");
  TEST_ASSERT_TRUE(settingsSanitize(s, DEV));
  TEST_ASSERT_EQUAL_STRING("home/sensor", s.mqttBase);
}

static void test_multiple_trailing_slashes_stripped(void) {
  Settings s = defaults();
  strcpy(s.mqttBase, "home/sensor///");
  TEST_ASSERT_TRUE(settingsSanitize(s, DEV));
  TEST_ASSERT_EQUAL_STRING("home/sensor", s.mqttBase);
}

static void test_base_of_only_slashes_restored(void) {
  // Строка из одних слэшей после обрезки опустела бы — база должна вернуться.
  Settings s = defaults();
  strcpy(s.mqttBase, "///");
  TEST_ASSERT_TRUE(settingsSanitize(s, DEV));
  TEST_ASSERT_EQUAL_STRING("watersensor/wps_abc123", s.mqttBase);
}

static void test_empty_device_name_restored(void) {
  Settings s = defaults();
  s.deviceName[0] = '\0';
  TEST_ASSERT_TRUE(settingsSanitize(s, DEV));
  TEST_ASSERT_EQUAL_STRING("Water Pressure", s.deviceName);
}

static void test_empty_ha_prefix_is_preserved(void) {
  // Регрессия: пустой префикс — штатный способ выключить Home Assistant.
  // Если валидация вернёт ему дефолт, настройка не переживёт перезагрузку.
  Settings s = defaults();
  s.haPrefix[0] = '\0';
  settingsSanitize(s, DEV);
  TEST_ASSERT_EQUAL_STRING("", s.haPrefix);
}

// -------------------------------------------------------------- калибровка ---

static void test_adc_scale_too_low_reset(void) {
  Settings s      = defaults();
  s.adcFullScaleV = 0.5f;
  TEST_ASSERT_TRUE(settingsSanitize(s, DEV));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.2f, s.adcFullScaleV);
}

static void test_adc_scale_too_high_reset(void) {
  Settings s      = defaults();
  s.adcFullScaleV = 10.1f;
  TEST_ASSERT_TRUE(settingsSanitize(s, DEV));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.2f, s.adcFullScaleV);
}

static void test_adc_scale_bounds_accepted(void) {
  Settings s      = defaults();
  s.adcFullScaleV = 5.0f;  // внешний делитель — законный случай
  TEST_ASSERT_FALSE(settingsSanitize(s, DEV));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f, s.adcFullScaleV);
}

static void test_zero_full_scale_pressure_reset(void) {
  Settings s = defaults();
  s.pFullMPa = 0.0f;
  TEST_ASSERT_TRUE(settingsSanitize(s, DEV));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, s.pFullMPa);
}

static void test_negative_full_scale_pressure_reset(void) {
  Settings s = defaults();
  s.pFullMPa = -1.0f;
  TEST_ASSERT_TRUE(settingsSanitize(s, DEV));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, s.pFullMPa);
}

static void test_equal_sensor_voltages_reset(void) {
  // Это и есть деление на ноль в pressureConvert(), если не поймать здесь.
  Settings s = defaults();
  s.vZero    = 2.0f;
  s.vFull    = 2.0f;
  TEST_ASSERT_TRUE(settingsSanitize(s, DEV));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, s.vZero);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.5f, s.vFull);
}

static void test_inverted_sensor_voltages_reset(void) {
  Settings s = defaults();
  s.vZero    = 4.5f;
  s.vFull    = 0.5f;
  TEST_ASSERT_TRUE(settingsSanitize(s, DEV));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, s.vZero);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.5f, s.vFull);
}

static void test_too_narrow_sensor_span_reset(void) {
  // Размах 0.05 В заведомо ниже порога 0.1 — делить на такое нельзя.
  // Проверять поведение ровно на границе (1.1f - 1.0f) бессмысленно: в float
  // эта разность равна 0.10000002, то есть уже больше 0.1f. Это тест на
  // представление чисел, а не на логику.
  Settings s = defaults();
  s.vZero    = 1.0f;
  s.vFull    = 1.05f;
  TEST_ASSERT_TRUE(settingsSanitize(s, DEV));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, s.vZero);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.5f, s.vFull);
}

static void test_sufficient_sensor_span_accepted(void) {
  // Нестандартный, но рабочий датчик с размахом 0.5 В трогать не нужно.
  Settings s = defaults();
  s.vZero    = 1.0f;
  s.vFull    = 1.5f;
  TEST_ASSERT_FALSE(settingsSanitize(s, DEV));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, s.vZero);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.5f, s.vFull);
}

static void test_nan_calibration_reset(void) {
  // NaN попадёт в настройки, если в поле портала ввели мусор: atof() вернёт 0,
  // но подстраховаться от NaN всё равно нужно — сравнения с ним всегда ложны.
  Settings s      = defaults();
  s.adcFullScaleV = NAN;
  s.pFullMPa      = NAN;
  s.vFull         = NAN;
  TEST_ASSERT_TRUE(settingsSanitize(s, DEV));
  TEST_ASSERT_FALSE(isnan(s.adcFullScaleV));
  TEST_ASSERT_FALSE(isnan(s.pFullMPa));
  TEST_ASSERT_FALSE(isnan(s.vFull));
}

// ------------------------------------------------- главная гарантия модуля ---

static void test_sanitize_always_yields_usable_math(void) {
  // Перебор заведомо испорченных комбинаций: после валидации пересчёт обязан
  // давать конечные числа при любом отсчёте.
  const float bad[] = {0.0f, -1.0f, NAN, 1e9f, 0.5f, 100.0f};
  const size_t n = sizeof(bad) / sizeof(bad[0]);

  for (size_t i = 0; i < n; i++) {
    for (size_t j = 0; j < n; j++) {
      for (size_t k = 0; k < n; k++) {
        Settings s      = defaults();
        s.adcFullScaleV = bad[i];
        s.vZero         = bad[j];
        s.vFull         = bad[k];
        s.pFullMPa      = bad[(i + j) % n];

        settingsSanitize(s, DEV);

        TEST_ASSERT_TRUE(s.vFull - s.vZero > 0.1f);
        TEST_ASSERT_TRUE(s.pFullMPa > 0.0f);

        Reading r = pressureConvert(512, s);
        TEST_ASSERT_FALSE(isnan(r.bar));
        TEST_ASSERT_FALSE(isinf(r.bar));
      }
    }
  }
}

static void test_sanitize_is_idempotent(void) {
  // Повторный прогон не должен ничего менять — иначе настройки «дрожали» бы
  // при каждом сохранении с портала.
  Settings s = defaults();
  s.mqttPort = 0;
  s.vFull    = 0.0f;
  strcpy(s.mqttBase, "home/");

  TEST_ASSERT_TRUE(settingsSanitize(s, DEV));

  Settings after = s;
  TEST_ASSERT_FALSE(settingsSanitize(s, DEV));
  TEST_ASSERT_EQUAL_MEMORY(&after, &s, sizeof(Settings));
}

int main(int, char **) {
  UNITY_BEGIN();

  RUN_TEST(test_defaults_disable_mqtt);
  RUN_TEST(test_defaults_base_topic_contains_device_id);
  RUN_TEST(test_defaults_match_xidibei_and_d1_mini);
  RUN_TEST(test_defaults_are_already_valid);
  RUN_TEST(test_defaults_handle_null_device_id);

  RUN_TEST(test_zero_port_reset);
  RUN_TEST(test_zero_interval_reset);
  RUN_TEST(test_huge_interval_reset);
  RUN_TEST(test_interval_bounds_are_inclusive);
  RUN_TEST(test_empty_base_topic_restored);
  RUN_TEST(test_trailing_slash_stripped);
  RUN_TEST(test_multiple_trailing_slashes_stripped);
  RUN_TEST(test_base_of_only_slashes_restored);
  RUN_TEST(test_empty_device_name_restored);
  RUN_TEST(test_empty_ha_prefix_is_preserved);

  RUN_TEST(test_adc_scale_too_low_reset);
  RUN_TEST(test_adc_scale_too_high_reset);
  RUN_TEST(test_adc_scale_bounds_accepted);
  RUN_TEST(test_zero_full_scale_pressure_reset);
  RUN_TEST(test_negative_full_scale_pressure_reset);
  RUN_TEST(test_equal_sensor_voltages_reset);
  RUN_TEST(test_inverted_sensor_voltages_reset);
  RUN_TEST(test_too_narrow_sensor_span_reset);
  RUN_TEST(test_sufficient_sensor_span_accepted);
  RUN_TEST(test_nan_calibration_reset);

  RUN_TEST(test_sanitize_always_yields_usable_math);
  RUN_TEST(test_sanitize_is_idempotent);

  return UNITY_END();
}
