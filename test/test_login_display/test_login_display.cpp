#include <unity.h>
#include "LoginDisplay.h"

void setUp() {}
void tearDown() {}

static void test_expiry_boundary_without_close_request() {
  LoginDisplay display;
  TEST_ASSERT_FALSE(display.visible(100));
  display.show(100);
  TEST_ASSERT_TRUE(display.visible(60099));
  TEST_ASSERT_FALSE(display.visible(60100));
  TEST_ASSERT_FALSE(display.visible(60101));
}
static void test_renewal_extends_password_display() {
  LoginDisplay display;
  display.show(100);
  display.show(20100);
  TEST_ASSERT_TRUE(display.visible(60100));
  TEST_ASSERT_FALSE(display.visible(80100));
}
static void test_hide_and_show_again() {
  LoginDisplay display;
  display.show(0); display.hide();
  TEST_ASSERT_FALSE(display.visible(1));
  display.show(2);
  TEST_ASSERT_TRUE(display.visible(2));
}
static void test_timeout_survives_millis_rollover() {
  LoginDisplay display;
  const uint32_t shown = UINT32_MAX - 100;
  display.show(shown);
  TEST_ASSERT_TRUE(display.visible(uint32_t(shown + 59999)));
  TEST_ASSERT_FALSE(display.visible(uint32_t(shown + 60000)));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_expiry_boundary_without_close_request);
  RUN_TEST(test_renewal_extends_password_display);
  RUN_TEST(test_hide_and_show_again);
  RUN_TEST(test_timeout_survives_millis_rollover);
  return UNITY_END();
}
