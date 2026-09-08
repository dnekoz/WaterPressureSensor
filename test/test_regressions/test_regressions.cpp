#include <unity.h>
#include <math.h>
#include <string.h>
#include <vector>
#include <string>
#include "Logic.h"

static Settings s;
void setUp() { s = Settings{}; settingsDefaults(s, "wps_test"); }
void tearDown() {}

static bool patch(const char *json) {
  JsonDocument doc;
  TEST_ASSERT_FALSE(bool(deserializeJson(doc, json)));
  char error[192];
  return settingsPatch(doc.as<JsonObjectConst>(), s, error, sizeof(error));
}
static void test_patch_is_transactional() {
  Settings before = s;
  TEST_ASSERT_FALSE(patch("{\"mqttHost\":\"new.example\",\"mqttPort\":65536}"));
  TEST_ASSERT_EQUAL_MEMORY(&before, &s, sizeof(s));
}
static void test_nonfinite_patch_rejected() {
  Settings before = s;
  TEST_ASSERT_FALSE(patch("{\"vFull\":1e99}"));
  TEST_ASSERT_EQUAL_MEMORY(&before, &s, sizeof(s));
  TEST_ASSERT_FALSE(patch("{\"pFullMPa\":1e99}"));
}
static void test_strict_types_and_unknown_fields() {
  TEST_ASSERT_FALSE(patch("{\"mqttPort\":\"1883\"}"));
  TEST_ASSERT_FALSE(patch("{\"mqttPort\":1883.1}"));
  TEST_ASSERT_FALSE(patch("{\"vZero\":null}"));
  TEST_ASSERT_FALSE(patch("{\"haPrefixApplied\":\"override\"}"));
  TEST_ASSERT_FALSE(patch("{\"mqttHost\":true}"));
}
static void test_topics_and_hosts_validated() {
  TEST_ASSERT_FALSE(patch("{\"mqttBase\":\"water/#\"}"));
  TEST_ASSERT_FALSE(patch("{\"mqttHost\":\"mqtt://host\"}"));
  TEST_ASSERT_FALSE(patch("{\"haPrefix\":\"ha/\"}"));
  TEST_ASSERT_TRUE(patch("{\"mqttHost\":\"192.168.1.10\",\"haPrefix\":\"\"}"));
}
static void test_patch_keeps_omitted_secrets() {
  strcpy(s.mqttPass, "existing-secret");
  TEST_ASSERT_TRUE(patch("{\"mqttPort\":8883,\"mqttTlsMode\":3}"));
  TEST_ASSERT_EQUAL_STRING("existing-secret", s.mqttPass);
  TEST_ASSERT_TRUE(patch("{\"mqttPass\":\"\"}"));
  TEST_ASSERT_EQUAL_STRING("", s.mqttPass);
}
static void test_portal_password_limits() {
  TEST_ASSERT_FALSE(patch("{\"portalPass\":\"short\"}"));
  TEST_ASSERT_FALSE(patch("{\"portalPass\":\"password with spaces\"}"));
  TEST_ASSERT_TRUE(patch("{\"portalPass\":\"long-password-123\"}"));
  JsonDocument doc;
  settingsToJson(doc.to<JsonObject>(), s);
  Settings loaded{}; settingsFromJson(doc.as<JsonObjectConst>(), loaded);
  TEST_ASSERT_EQUAL_STRING(s.portalPass, loaded.portalPass);
}
static void test_utf8_byte_limit() {
  TEST_ASSERT_FALSE(patch("{\"deviceName\":\"01234567890123456789012345678901\"}"));
  TEST_ASSERT_TRUE(patch("{\"deviceName\":\"Давление воды\"}"));
}
static void test_sanitize_infinity() {
  s.vFull = INFINITY; s.pFullMPa = INFINITY; s.vZero = -INFINITY;
  TEST_ASSERT_TRUE(settingsSanitize(s, "wps_test"));
  TEST_ASSERT_NULL(settingsValidationError(s));
  TEST_ASSERT_TRUE(isfinite(pressureConvert(800, s).bar));
}
static void test_external_resistor_range() {
  TEST_ASSERT_TRUE(patch("{\"adcFullScaleV\":5.4}"));
  TEST_ASSERT_FLOAT_WITHIN(.001f, 5.0f, pressureMaxBar(s));
  TEST_ASSERT_FALSE(pressureConvert(852, s).over); // ~4.497 V, full sensor range without ADC clipping
  TEST_ASSERT_TRUE(pressureConvert(900, s).over); // sensor voltage outside its curve
}
static void test_short_clip_detected_before_average() {
  PressureFilter filter;
  pressureFilterSample(filter, 700, 100, s);
  pressureFilterSample(filter, 1023, 120, s);
  auto r = pressureFilterRead(filter, 120, s);
  TEST_ASSERT_TRUE(r.raw < SENSOR_RAW_OVER);
  TEST_ASSERT_TRUE(r.over);
  TEST_ASSERT_EQUAL_INT(1023, r.rawInstant);
}
static void test_short_disconnect_detected_before_average() {
  PressureFilter filter;
  pressureFilterSample(filter, 700, 100, s);
  pressureFilterSample(filter, 0, 120, s);
  TEST_ASSERT_TRUE(pressureFilterRead(filter, 120, s).fault);
}
static void test_fault_recovers_only_after_stable_second() {
  PressureFilter filter;
  pressureFilterSample(filter, 1023, 100, s);
  for (uint32_t now = 120; now < 1100; now += 20) {
    pressureFilterSample(filter, 700, now, s);
    TEST_ASSERT_TRUE(pressureFilterRead(filter, now, s).over);
  }
  pressureFilterSample(filter, 700, 1100, s);
  TEST_ASSERT_FALSE(pressureFilterRead(filter, 1100, s).over);
}
static void test_fresh_sample_replaces_old_window_after_stall() {
  PressureFilter filter;
  pressureFilterSample(filter, 800, 100, s);
  TEST_ASSERT_TRUE(pressureFilterRead(filter, 1100, s).stale);
  pressureFilterSample(filter, 500, 5100, s);
  auto r = pressureFilterRead(filter, 5120, s);
  TEST_ASSERT_EQUAL_INT(500, r.raw);
  TEST_ASSERT_EQUAL_UINT32(20, r.ageMs);
  TEST_ASSERT_FALSE(r.stale);
}
static void test_filter_millis_wrap() {
  PressureFilter filter;
  pressureFilterSample(filter, 800, UINT32_MAX - 10, s);
  auto r = pressureFilterRead(filter, 9, s);
  TEST_ASSERT_EQUAL_UINT32(20, r.ageMs);
  TEST_ASSERT_FALSE(r.stale);
}
static void test_gap_does_not_count_as_healthy_recovery() {
  PressureFilter filter;
  pressureFilterSample(filter, 1023, 100, s);
  pressureFilterSample(filter, 700, 5100, s);
  TEST_ASSERT_TRUE(pressureFilterRead(filter, 5100, s).over);
  for (uint32_t now = 5120; now <= 6100; now += 20) pressureFilterSample(filter, 700, now, s);
  TEST_ASSERT_FALSE(pressureFilterRead(filter, 6100, s).over);
}
static void test_stale_and_nonfinite_pressure_serializes_as_null() {
  Reading r = pressureConvert(800, s); r.stale = true;
  char buffer[320]; JsonDocument doc;
  TEST_ASSERT_TRUE(mqttStateJson(buffer, sizeof(buffer), r, -60, 30000) > 0);
  TEST_ASSERT_FALSE(bool(deserializeJson(doc, buffer)));
  TEST_ASSERT_TRUE(doc["bar"].isNull());
  TEST_ASSERT_TRUE(doc["stale"].as<bool>());
  r.stale = false; r.bar = INFINITY; r.volts = NAN;
  mqttStateJson(buffer, sizeof(buffer), r, -60, 30000);
  TEST_ASSERT_FALSE(bool(deserializeJson(doc, buffer)));
  TEST_ASSERT_TRUE(doc["bar"].isNull());
  TEST_ASSERT_TRUE(doc["volt"].isNull());
}
static void test_too_small_json_buffer_fails_closed() {
  char buffer[10]; memset(buffer, 'x', sizeof(buffer));
  TEST_ASSERT_EQUAL_size_t(0, mqttStateJson(buffer, sizeof(buffer), pressureConvert(800, s), -60, 30000));
  TEST_ASSERT_EQUAL_CHAR('\0', buffer[0]);
}
struct Sink {
  int failAt = -1;
  bool diskOk = true;
  int persists = 0;
  std::vector<std::string> topics;
};
static bool publish(const char *prefix, const char *key, bool remove, void *context) {
  Sink &sink = *(Sink *)context;
  sink.topics.push_back(std::string(remove ? "delete:" : "publish:") + prefix + "/" + key);
  return int(sink.topics.size()) != sink.failAt;
}
static bool persist(const Settings &, void *context) {
  Sink &sink = *(Sink *)context; sink.persists++; return sink.diskOk;
}
static void test_discovery_failure_keeps_old_prefix_for_retry() {
  strcpy(s.haPrefixApplied, "old"); strcpy(s.haPrefix, "new");
  Sink sink; sink.failAt = 2;
  TEST_ASSERT_FALSE(mqttReconcileDiscovery(s, publish, persist, &sink));
  TEST_ASSERT_EQUAL_STRING("old", s.haPrefixApplied);
  TEST_ASSERT_EQUAL_INT(0, sink.persists);
  sink.failAt = -1; sink.topics.clear();
  TEST_ASSERT_TRUE(mqttReconcileDiscovery(s, publish, persist, &sink));
  TEST_ASSERT_EQUAL_STRING("new", s.haPrefixApplied);
  TEST_ASSERT_EQUAL_STRING("delete:old/pressure", sink.topics[0].c_str());
  TEST_ASSERT_EQUAL_STRING("publish:new/pressure", sink.topics[4].c_str());
}
static void test_discovery_failed_persistence_retries_cleanup() {
  strcpy(s.haPrefixApplied, "old"); s.haPrefix[0] = '\0';
  Sink sink; sink.diskOk = false;
  TEST_ASSERT_FALSE(mqttReconcileDiscovery(s, publish, persist, &sink));
  TEST_ASSERT_EQUAL_STRING("old", s.haPrefixApplied);
  sink.diskOk = true;
  TEST_ASSERT_TRUE(mqttReconcileDiscovery(s, publish, persist, &sink));
  TEST_ASSERT_EQUAL_STRING("", s.haPrefixApplied);
}
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_patch_is_transactional);
  RUN_TEST(test_nonfinite_patch_rejected);
  RUN_TEST(test_strict_types_and_unknown_fields);
  RUN_TEST(test_topics_and_hosts_validated);
  RUN_TEST(test_patch_keeps_omitted_secrets);
  RUN_TEST(test_portal_password_limits);
  RUN_TEST(test_utf8_byte_limit);
  RUN_TEST(test_sanitize_infinity);
  RUN_TEST(test_external_resistor_range);
  RUN_TEST(test_short_clip_detected_before_average);
  RUN_TEST(test_short_disconnect_detected_before_average);
  RUN_TEST(test_fault_recovers_only_after_stable_second);
  RUN_TEST(test_fresh_sample_replaces_old_window_after_stall);
  RUN_TEST(test_filter_millis_wrap);
  RUN_TEST(test_gap_does_not_count_as_healthy_recovery);
  RUN_TEST(test_stale_and_nonfinite_pressure_serializes_as_null);
  RUN_TEST(test_too_small_json_buffer_fails_closed);
  RUN_TEST(test_discovery_failure_keeps_old_prefix_for_retry);
  RUN_TEST(test_discovery_failed_persistence_retries_cleanup);
  return UNITY_END();
}
