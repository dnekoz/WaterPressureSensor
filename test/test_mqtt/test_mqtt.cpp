// Тесты сборки MQTT-топиков и payload.
//
// Самый важный тест здесь — test_discovery_fits_in_publish_buffer: у
// PubSubClient буфер фиксированный, и при переполнении publish() молча
// возвращает false, то есть Home Assistant просто не увидит устройство.

#include <unity.h>

#include <string.h>

#include "Logic.h"

// Должно совпадать с MQTT_BUFFER_SIZE из src/Config.h. Config.h сюда не
// включаем: он тянет Arduino-специфику и на хосте не соберётся.
static const size_t PUBLISH_BUFFER = 1024;

static const char *DEV = "wps_abc123";

static Settings defaults() {
  Settings s;
  settingsDefaults(s, DEV);
  return s;
}

static Reading makeReading(int raw, float volts, float mpa, float bar, bool over, bool fault) {
  Reading r;
  r.raw   = raw;
  r.volts = volts;
  r.mpa   = mpa;
  r.bar   = bar;
  r.over  = over;
  r.fault = fault;
  return r;
}

void setUp(void) {}
void tearDown(void) {}

// ------------------------------------------------------------------ топики ---

static void test_state_topic(void) {
  char buf[96];
  mqttTopic(buf, sizeof(buf), defaults(), "state");
  TEST_ASSERT_EQUAL_STRING("watersensor/wps_abc123/state", buf);
}

static void test_status_topic(void) {
  char buf[96];
  mqttTopic(buf, sizeof(buf), defaults(), "status");
  TEST_ASSERT_EQUAL_STRING("watersensor/wps_abc123/status", buf);
}

static void test_custom_base_topic(void) {
  Settings s = defaults();
  strcpy(s.mqttBase, "home/boiler");

  char buf[96];
  mqttTopic(buf, sizeof(buf), s, "state");
  TEST_ASSERT_EQUAL_STRING("home/boiler/state", buf);
}

static void test_topic_does_not_overflow_small_buffer(void) {
  // snprintf обязан обрезать и оставить завершающий ноль, а не затереть стек.
  char buf[10];
  memset(buf, 'X', sizeof(buf));
  mqttTopic(buf, sizeof(buf), defaults(), "state");
  TEST_ASSERT_EQUAL_CHAR('\0', buf[sizeof(buf) - 1]);
  TEST_ASSERT_TRUE(strlen(buf) < sizeof(buf));
}

static void test_discovery_topic(void) {
  char buf[160];
  mqttDiscoveryTopic(buf, sizeof(buf), defaults(), DEV, "pressure");
  TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/wps_abc123/pressure/config", buf);
}

static void test_discovery_topic_with_custom_prefix(void) {
  Settings s = defaults();
  strcpy(s.haPrefix, "ha");

  char buf[160];
  mqttDiscoveryTopic(buf, sizeof(buf), s, DEV, "rssi");
  TEST_ASSERT_EQUAL_STRING("ha/sensor/wps_abc123/rssi/config", buf);
}

// -------------------------------------------------------- payload состояния ---

static void test_state_json_normal(void) {
  Reading r = makeReading(812, 2.53997f, 0.254996f, 2.54996f, false, false);

  char buf[256];
  size_t len = mqttStateJson(buf, sizeof(buf), r, -58, 24568);

  TEST_ASSERT_EQUAL_STRING(
      "{\"bar\":2.55,\"mpa\":0.255,\"volt\":2.540,\"raw\":812,\"rssi\":-58,"
      "\"over\":false,\"fault\":false,\"heap\":24568}",
      buf);
  TEST_ASSERT_EQUAL_size_t(strlen(buf), len);
}

static void test_state_json_rounds_to_two_decimals(void) {
  Reading r = makeReading(500, 1.5f, 0.125f, 1.256789f, false, false);

  char buf[256];
  mqttStateJson(buf, sizeof(buf), r, -40, 20000);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"bar\":1.26"));
}

static void test_state_json_nulls_pressure_when_over(void) {
  // Давление выше диапазона неизвестно — публиковать число нельзя,
  // иначе Home Assistant нарисует на графике выдуманное значение.
  Reading r = makeReading(1023, 3.2f, 0.3375f, 3.375f, true, false);

  char buf[256];
  mqttStateJson(buf, sizeof(buf), r, -58, 24568);

  TEST_ASSERT_NOT_NULL(strstr(buf, "\"bar\":null"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"mpa\":null"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"over\":true"));
}

static void test_state_json_nulls_pressure_when_fault(void) {
  Reading r = makeReading(0, 0.0f, -0.0625f, -0.625f, false, true);

  char buf[256];
  mqttStateJson(buf, sizeof(buf), r, -58, 24568);

  TEST_ASSERT_NOT_NULL(strstr(buf, "\"bar\":null"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"mpa\":null"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"fault\":true"));
}

static void test_state_json_keeps_raw_measurements_on_fault(void) {
  // Напряжение и сырой отсчёт измерены честно и помогают понять причину —
  // они должны публиковаться даже при обрыве.
  Reading r = makeReading(3, 0.009f, -0.0614f, -0.614f, false, true);

  char buf[256];
  mqttStateJson(buf, sizeof(buf), r, -70, 12345);

  TEST_ASSERT_NOT_NULL(strstr(buf, "\"raw\":3"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"volt\":0.009"));
  TEST_ASSERT_NULL(strstr(buf, "\"volt\":null"));
}

static void test_state_json_negative_rssi(void) {
  Reading r = makeReading(512, 1.6f, 0.1376f, 1.376f, false, false);

  char buf[256];
  mqttStateJson(buf, sizeof(buf), r, -91, 1024);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"rssi\":-91"));
}

static void test_state_json_fits_in_publish_buffer(void) {
  Reading r = makeReading(1023, 3.2f, 0.3375f, 3.375f, false, false);

  char buf[512];
  size_t len = mqttStateJson(buf, sizeof(buf), r, -100, 4294967295u);
  TEST_ASSERT_TRUE(len < PUBLISH_BUFFER);
}

// -------------------------------------------------------- payload discovery ---

static void test_discovery_pressure_entity(void) {
  char buf[1024];
  mqttDiscoveryJson(buf, sizeof(buf), defaults(), DEV, "pressure", "Pressure", "pressure", "bar",
                    "{{ value_json.bar }}", false);

  TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"Pressure\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"uniq_id\":\"wps_abc123_pressure\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"stat_t\":\"watersensor/wps_abc123/state\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"avty_t\":\"watersensor/wps_abc123/status\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"dev_cla\":\"pressure\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"unit_of_meas\":\"bar\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"stat_cla\":\"measurement\""));
}

static void test_discovery_value_template_is_escaped_correctly(void) {
  char buf[1024];
  mqttDiscoveryJson(buf, sizeof(buf), defaults(), DEV, "pressure", "Pressure", "pressure", "bar",
                    "{{ value_json.bar }}", false);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"val_tpl\":\"{{ value_json.bar }}\""));
}

static void test_discovery_device_block_groups_entities(void) {
  // Общий блock device — то, что склеивает четыре сущности в одно устройство
  // в интерфейсе Home Assistant.
  char buf[1024];
  mqttDiscoveryJson(buf, sizeof(buf), defaults(), DEV, "pressure", "Pressure", "pressure", "bar",
                    "{{ value_json.bar }}", false);

  TEST_ASSERT_NOT_NULL(strstr(buf, "\"ids\":[\"wps_abc123\"]"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"Water Pressure\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"mdl\":\"Wemos D1 mini\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"mf\":\"Espressif\""));
}

static void test_discovery_uses_custom_device_name(void) {
  Settings s = defaults();
  strcpy(s.deviceName, "Boiler loop");

  char buf[1024];
  mqttDiscoveryJson(buf, sizeof(buf), s, DEV, "pressure", "Pressure", "pressure", "bar",
                    "{{ value_json.bar }}", false);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"Boiler loop\""));
}

static void test_discovery_diagnostic_flag(void) {
  char buf[1024];

  mqttDiscoveryJson(buf, sizeof(buf), defaults(), DEV, "rssi", "WiFi signal", "signal_strength",
                    "dBm", "{{ value_json.rssi }}", true);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"ent_cat\":\"diagnostic\""));

  mqttDiscoveryJson(buf, sizeof(buf), defaults(), DEV, "pressure", "Pressure", "pressure", "bar",
                    "{{ value_json.bar }}", false);
  TEST_ASSERT_NULL(strstr(buf, "ent_cat"));
}

static void test_discovery_omits_optional_keys(void) {
  // Сущность «сырой отсчёт АЦП» не имеет ни device_class, ни единицы измерения.
  char buf[1024];
  mqttDiscoveryJson(buf, sizeof(buf), defaults(), DEV, "raw", "ADC raw", nullptr, nullptr,
                    "{{ value_json.raw }}", true);

  TEST_ASSERT_NULL(strstr(buf, "dev_cla"));
  TEST_ASSERT_NULL(strstr(buf, "unit_of_meas"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"uniq_id\":\"wps_abc123_raw\""));
}

static void test_discovery_unique_ids_differ_per_entity(void) {
  char a[1024];
  char b[1024];
  mqttDiscoveryJson(a, sizeof(a), defaults(), DEV, "pressure", "Pressure", "pressure", "bar",
                    "{{ value_json.bar }}", false);
  mqttDiscoveryJson(b, sizeof(b), defaults(), DEV, "voltage", "Sensor voltage", "voltage", "V",
                    "{{ value_json.volt }}", false);

  TEST_ASSERT_NOT_NULL(strstr(a, "wps_abc123_pressure"));
  TEST_ASSERT_NOT_NULL(strstr(b, "wps_abc123_voltage"));
  TEST_ASSERT_NOT_EQUAL(0, strcmp(a, b));
}

static void test_discovery_fits_in_publish_buffer(void) {
  // Ради этого теста всё и затевалось: при переполнении буфера PubSubClient
  // publish() возвращает false молча, и устройство не появится в Home Assistant.
  // Проверяем все четыре сущности, причём с длинными настройками.
  Settings s = defaults();
  strcpy(s.mqttBase, "very/long/base/topic/for/this/water/pressure/sensor/device");
  strcpy(s.deviceName, "Water Pressure Sensor In The Boiler Room");
  strcpy(s.haPrefix, "homeassistant");

  struct Entity {
    const char *key;
    const char *name;
    const char *devClass;
    const char *unit;
    const char *tmpl;
    bool        diagnostic;
  };

  const Entity entities[] = {
      {"pressure", "Pressure", "pressure", "bar", "{{ value_json.bar }}", false},
      {"voltage", "Sensor voltage", "voltage", "V", "{{ value_json.volt }}", false},
      {"rssi", "WiFi signal", "signal_strength", "dBm", "{{ value_json.rssi }}", true},
      {"raw", "ADC raw", nullptr, nullptr, "{{ value_json.raw }}", true},
  };

  for (size_t i = 0; i < sizeof(entities) / sizeof(entities[0]); i++) {
    char   buf[2048];
    size_t len = mqttDiscoveryJson(buf, sizeof(buf), s, DEV, entities[i].key, entities[i].name,
                                   entities[i].devClass, entities[i].unit, entities[i].tmpl,
                                   entities[i].diagnostic);
    TEST_ASSERT_TRUE_MESSAGE(len > 0, entities[i].key);
    TEST_ASSERT_TRUE_MESSAGE(len < PUBLISH_BUFFER, entities[i].key);
  }
}

int main(int, char **) {
  UNITY_BEGIN();

  RUN_TEST(test_state_topic);
  RUN_TEST(test_status_topic);
  RUN_TEST(test_custom_base_topic);
  RUN_TEST(test_topic_does_not_overflow_small_buffer);
  RUN_TEST(test_discovery_topic);
  RUN_TEST(test_discovery_topic_with_custom_prefix);

  RUN_TEST(test_state_json_normal);
  RUN_TEST(test_state_json_rounds_to_two_decimals);
  RUN_TEST(test_state_json_nulls_pressure_when_over);
  RUN_TEST(test_state_json_nulls_pressure_when_fault);
  RUN_TEST(test_state_json_keeps_raw_measurements_on_fault);
  RUN_TEST(test_state_json_negative_rssi);
  RUN_TEST(test_state_json_fits_in_publish_buffer);

  RUN_TEST(test_discovery_pressure_entity);
  RUN_TEST(test_discovery_value_template_is_escaped_correctly);
  RUN_TEST(test_discovery_device_block_groups_entities);
  RUN_TEST(test_discovery_uses_custom_device_name);
  RUN_TEST(test_discovery_diagnostic_flag);
  RUN_TEST(test_discovery_omits_optional_keys);
  RUN_TEST(test_discovery_unique_ids_differ_per_entity);
  RUN_TEST(test_discovery_fits_in_publish_buffer);

  return UNITY_END();
}
