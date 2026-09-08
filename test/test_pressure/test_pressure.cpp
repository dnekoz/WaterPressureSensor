// Тесты пересчёта отсчёта АЦП в давление.
//
// Опорные числа посчитаны вручную по схеме «XIDIBEI напрямую на A0 у D1 mini»:
//   volts = raw / 1023 * 3.2      (делитель платы 220k/100k)
//   mpa   = (volts - 0.5) * 0.5 / (4.5 - 0.5)
//   bar   = mpa * 10

#include <unity.h>

#include <math.h>

#include "Logic.h"

static Settings defaults() {
  Settings s;
  settingsDefaults(s, "wps_abc123");
  return s;
}

void setUp(void) {}
void tearDown(void) {}

// --------------------------------------------------------------- напряжение ---

static void test_volts_at_zero_raw(void) {
  Reading r = pressureConvert(0, defaults());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, r.volts);
}

static void test_volts_at_full_raw(void) {
  // Отсчёт 1023 соответствует 3.2 В на пине A0 — это и есть потолок платы.
  Reading r = pressureConvert(1023, defaults());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.2f, r.volts);
}

static void test_volts_is_linear_in_raw(void) {
  Settings s = defaults();
  Reading  a = pressureConvert(256, s);
  Reading  b = pressureConvert(512, s);
  TEST_ASSERT_FLOAT_WITHIN(0.002f, 2.0f * a.volts, b.volts);
}

// ------------------------------------------------------------------ давление ---

static void test_zero_pressure_at_sensor_minimum(void) {
  // 0.5 В — нижняя точка кривой датчика, это ровно нулевое давление.
  // raw = 0.5 * 1023 / 3.2 = 159.84, ближайший отсчёт 160.
  Reading r = pressureConvert(160, defaults());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, r.mpa);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, r.bar);
}

static void test_typical_reading(void) {
  // raw=812 -> volts = 812*3.2/1023 = 2.53997 В
  //         -> mpa   = (2.53997-0.5)*0.125 = 0.254996
  Reading r = pressureConvert(812, defaults());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.53997f, r.volts);
  TEST_ASSERT_FLOAT_WITHIN(0.0005f, 0.254996f, r.mpa);
  TEST_ASSERT_FLOAT_WITHIN(0.005f, 2.54996f, r.bar);
}

static void test_bar_is_ten_times_mpa(void) {
  Settings s = defaults();
  for (int raw = 0; raw <= 1023; raw += 97) {
    Reading r = pressureConvert(raw, s);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, r.mpa * 10.0f, r.bar);
  }
}

static void test_pressure_below_sensor_zero_is_negative(void) {
  // Отрицательное давление — осмысленный сигнал: датчик выдаёт меньше 0.5 В,
  // то есть либо оборван, либо разбалансирован.
  Reading r = pressureConvert(0, defaults());
  TEST_ASSERT_TRUE(r.mpa < 0.0f);
}

// ------------------------------------------------------- граница диапазона ---

static void test_max_bar_with_defaults(void) {
  // (3.2 - 0.5) * 0.5 / 4.0 * 10 = 3.375 бар — потолок этой схемы без делителя.
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.375f, pressureMaxBar(defaults()));
}

static void test_reading_at_raw_1023_matches_max_bar(void) {
  Settings s = defaults();
  Reading  r = pressureConvert(1023, s);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, pressureMaxBar(s), r.bar);
}

// ------------------------------------------------------------- флаг over ---

static void test_over_flag_below_threshold(void) {
  TEST_ASSERT_FALSE(pressureConvert(SENSOR_RAW_OVER - 1, defaults()).over);
}

static void test_over_flag_at_threshold(void) {
  TEST_ASSERT_TRUE(pressureConvert(SENSOR_RAW_OVER, defaults()).over);
}

static void test_over_flag_at_max_raw(void) {
  TEST_ASSERT_TRUE(pressureConvert(1023, defaults()).over);
}

static void test_normal_reading_is_not_over(void) {
  TEST_ASSERT_FALSE(pressureConvert(812, defaults()).over);
}

// ------------------------------------------------------------- флаг fault ---

static void test_fault_when_input_floating(void) {
  // Отключённый датчик: на A0 около нуля.
  TEST_ASSERT_TRUE(pressureConvert(0, defaults()).fault);
}

static void test_fault_just_below_threshold(void) {
  // Порог 0.5 - 0.2 = 0.3 В, это raw = 0.3*1023/3.2 = 95.9.
  // raw=95 -> 0.29716 В, ниже порога.
  TEST_ASSERT_TRUE(pressureConvert(95, defaults()).fault);
}

static void test_no_fault_just_above_threshold(void) {
  // raw=96 -> 0.30029 В, уже выше порога.
  TEST_ASSERT_FALSE(pressureConvert(96, defaults()).fault);
}

static void test_normal_reading_is_not_fault(void) {
  TEST_ASSERT_FALSE(pressureConvert(812, defaults()).fault);
}

// ------------------------------------------------- произвольная калибровка ---

static void test_five_mpa_sensor(void) {
  // Тот же датчик, но на 5 МПа: важно, что шкала берётся из настроек,
  // а не зашита в код. В README это поле «Полная шкала датчика».
  Settings s = defaults();
  s.pFullMPa = 5.0f;

  Reading r = pressureConvert(812, s);
  TEST_ASSERT_FLOAT_WITHIN(0.005f, 2.549963f, r.mpa);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 25.49963f, r.bar);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 33.75f, pressureMaxBar(s));
}

static void test_external_divider_calibration(void) {
  // Если перед A0 поставили внешний делитель, потолок задаётся полем
  // «Напряжение на A0 при отсчёте 1023». Делитель ровно под датчик (4.5 В)
  // делает потолок платы равным полной шкале датчика.
  Settings s = defaults();
  s.adcFullScaleV = 4.5f;

  Reading r = pressureConvert(1023, s);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.5f, r.volts);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, r.mpa);  // ровно полная шкала датчика
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 5.0f, pressureMaxBar(s));
}

static void test_adc_scale_above_sensor_range(void) {
  // Делитель под 5 В — выше паспортных 4.5 В датчика. Пересчёт честно выдаёт
  // значение за пределами шкалы датчика: (5.0-0.5)*0.5/4.0 = 0.5625 МПа.
  // Это не ошибка расчёта, а следствие калибровки — датчик столько не выдаст.
  Settings s = defaults();
  s.adcFullScaleV = 5.0f;

  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5625f, pressureConvert(1023, s).mpa);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 5.0f, pressureMaxBar(s));
  TEST_ASSERT_TRUE(pressureConvert(1023, s).over);
}

static void test_shifted_sensor_zero(void) {
  // Датчик с нулём 0.45 В вместо 0.5 В: сдвигается и ноль, и порог обрыва.
  Settings s = defaults();
  s.vZero = 0.45f;

  Reading r = pressureConvert(144, s);  // 144*3.2/1023 = 0.45044 В
  TEST_ASSERT_FLOAT_WITHIN(0.002f, 0.0f, r.bar);
  TEST_ASSERT_FALSE(r.fault);
}

// ------------------------------------------------------------- устойчивость ---

static void test_no_nan_across_full_range(void) {
  // Ни один отсчёт не должен давать NaN или бесконечность: настройки к этому
  // моменту всегда прошли settingsSanitize().
  Settings s = defaults();
  for (int raw = 0; raw <= 1023; raw++) {
    Reading r = pressureConvert(raw, s);
    TEST_ASSERT_FALSE(isnan(r.bar));
    TEST_ASSERT_FALSE(isinf(r.bar));
    TEST_ASSERT_FALSE(isnan(r.volts));
  }
}

static void test_monotonic_in_raw(void) {
  // Давление обязано расти вместе с отсчётом — защита от опечатки в знаке.
  Settings s    = defaults();
  float    prev = pressureConvert(0, s).bar;
  for (int raw = 1; raw <= 1023; raw++) {
    float cur = pressureConvert(raw, s).bar;
    TEST_ASSERT_TRUE(cur > prev);
    prev = cur;
  }
}

int main(int, char **) {
  UNITY_BEGIN();

  RUN_TEST(test_volts_at_zero_raw);
  RUN_TEST(test_volts_at_full_raw);
  RUN_TEST(test_volts_is_linear_in_raw);

  RUN_TEST(test_zero_pressure_at_sensor_minimum);
  RUN_TEST(test_typical_reading);
  RUN_TEST(test_bar_is_ten_times_mpa);
  RUN_TEST(test_pressure_below_sensor_zero_is_negative);

  RUN_TEST(test_max_bar_with_defaults);
  RUN_TEST(test_reading_at_raw_1023_matches_max_bar);

  RUN_TEST(test_over_flag_below_threshold);
  RUN_TEST(test_over_flag_at_threshold);
  RUN_TEST(test_over_flag_at_max_raw);
  RUN_TEST(test_normal_reading_is_not_over);

  RUN_TEST(test_fault_when_input_floating);
  RUN_TEST(test_fault_just_below_threshold);
  RUN_TEST(test_no_fault_just_above_threshold);
  RUN_TEST(test_normal_reading_is_not_fault);

  RUN_TEST(test_five_mpa_sensor);
  RUN_TEST(test_external_divider_calibration);
  RUN_TEST(test_adc_scale_above_sensor_range);
  RUN_TEST(test_shifted_sensor_zero);

  RUN_TEST(test_no_nan_across_full_range);
  RUN_TEST(test_monotonic_in_raw);

  return UNITY_END();
}
