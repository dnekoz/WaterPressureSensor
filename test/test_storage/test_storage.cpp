#include <unity.h>
#include "../fakes/LittleFS.h"
// Compile production persistence code against a fault-injecting filesystem.
#include "../../src/Settings.cpp"
#include "../fakes/BearSSLHelpers.h"
namespace ca_test {
#include "../../src/CaStore.cpp"
}
using namespace ca_test;

static const char *OLD_CA = "-----BEGIN CERTIFICATE-----\nold\n-----END CERTIFICATE-----\n";
static const char *NEW_CA = "-----BEGIN CERTIFICATE-----\nnew\n-----END CERTIFICATE-----\n";
void setUp() {
  faults = Faults{}; LittleFS.files.clear();
  settingsDefaults(settings, "wps_test");
  strcpy(settings.portalPass, "persistent-secret");
  TEST_ASSERT_TRUE(settingsSave());
  auto file = LittleFS.open("/ca.pem", "w"); file.write((const uint8_t *)OLD_CA, strlen(OLD_CA)); file.close();
}
void tearDown() { faults = Faults{}; caWriteAbort(); }
static void test_settings_success_replaces_and_roundtrips_float() {
  Settings candidate = settings; candidate.vZero = .51234f; strcpy(candidate.deviceName, "New name");
  TEST_ASSERT_TRUE(settingsCommit(candidate));
  TEST_ASSERT_EQUAL_STRING("New name", settings.deviceName);
  settingsBegin();
  TEST_ASSERT_EQUAL_STRING("New name", settings.deviceName);
  TEST_ASSERT_FLOAT_WITHIN(.000001f, .51234f, settings.vZero);
  TEST_ASSERT_EQUAL_STRING("persistent-secret", settings.portalPass);
}
static void failedSettingsSavePreservesBoth() {
  std::string old = LittleFS.contents("/config.json");
  Settings before = settings, candidate = settings; strcpy(candidate.deviceName, "Rejected");
  TEST_ASSERT_FALSE(settingsCommit(candidate));
  TEST_ASSERT_EQUAL_STRING(old.c_str(), LittleFS.contents("/config.json").c_str());
  TEST_ASSERT_EQUAL_MEMORY(&before, &settings, sizeof(settings));
  TEST_ASSERT_TRUE(strlen(settingsLastError()) > 0);
}
static void test_short_write_preserves_settings() { faults.limit = 53; failedSettingsSavePreservesBoth(); }
static void test_corrupt_write_preserves_settings() { faults.corrupt = true; failedSettingsSavePreservesBoth(); }
static void test_truncated_flush_preserves_settings() { faults.truncateOnClose = true; failedSettingsSavePreservesBoth(); }
static void test_failed_rename_preserves_settings() { faults.rename = true; failedSettingsSavePreservesBoth(); }
static void test_corrupt_primary_recovers_temporary() {
  LittleFS.files["/config.tmp"] = std::make_shared<std::string>(LittleFS.contents("/config.json"));
  LittleFS.files["/config.json"] = std::make_shared<std::string>("{broken");
  settingsBegin();
  TEST_ASSERT_EQUAL_STRING("persistent-secret", settings.portalPass);
  TEST_ASSERT_FALSE(LittleFS.exists("/config.tmp"));
}
static void test_ca_success_replaces_old() {
  TEST_ASSERT_TRUE(caWriteBegin());
  TEST_ASSERT_TRUE(caWriteChunk((const uint8_t *)NEW_CA, strlen(NEW_CA)));
  TEST_ASSERT_TRUE(caWriteEnd());
  TEST_ASSERT_EQUAL_STRING(NEW_CA, LittleFS.contents("/ca.pem").c_str());
}
static void test_ca_rename_failure_preserves_old() {
  faults.rename = true;
  TEST_ASSERT_TRUE(caWriteBegin());
  TEST_ASSERT_TRUE(caWriteChunk((const uint8_t *)NEW_CA, strlen(NEW_CA)));
  TEST_ASSERT_FALSE(caWriteEnd());
  TEST_ASSERT_EQUAL_STRING(OLD_CA, LittleFS.contents("/ca.pem").c_str());
}
static void test_ca_abort_and_short_write_preserve_old() {
  TEST_ASSERT_TRUE(caWriteBegin()); faults.limit = 12;
  TEST_ASSERT_FALSE(caWriteChunk((const uint8_t *)NEW_CA, strlen(NEW_CA)));
  caWriteAbort();
  TEST_ASSERT_EQUAL_STRING(OLD_CA, LittleFS.contents("/ca.pem").c_str());
}
static void test_ca_truncated_close_preserves_old() {
  TEST_ASSERT_TRUE(caWriteBegin());
  TEST_ASSERT_TRUE(caWriteChunk((const uint8_t *)NEW_CA, strlen(NEW_CA)));
  faults.truncateOnClose = true;
  TEST_ASSERT_FALSE(caWriteEnd());
  TEST_ASSERT_EQUAL_STRING(OLD_CA, LittleFS.contents("/ca.pem").c_str());
}
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_settings_success_replaces_and_roundtrips_float);
  RUN_TEST(test_short_write_preserves_settings);
  RUN_TEST(test_corrupt_write_preserves_settings);
  RUN_TEST(test_truncated_flush_preserves_settings);
  RUN_TEST(test_failed_rename_preserves_settings);
  RUN_TEST(test_corrupt_primary_recovers_temporary);
  RUN_TEST(test_ca_success_replaces_old);
  RUN_TEST(test_ca_rename_failure_preserves_old);
  RUN_TEST(test_ca_abort_and_short_write_preserve_old);
  RUN_TEST(test_ca_truncated_close_preserves_old);
  return UNITY_END();
}
