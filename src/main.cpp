#include <Arduino.h>

#include "Config.h"
#include "Display.h"
#include "Mqtt.h"
#include "Portal.h"
#include "Sensor.h"
#include "Settings.h"

static unsigned long s_lastSampleMs  = 0;
static unsigned long s_lastDisplayMs = 0;
static unsigned long s_lastPublishMs = 0;

void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println();
  Serial.println(F(FW_NAME " v" FW_VERSION));

  // Настройки читаются первыми: от них зависят и калибровка датчика,
  // и значения полей на портале.
  settingsBegin();

  displayBegin();
  displayBoot();
  delay(1000);

  sensorBegin();
  portalBegin();
  mqttBegin();

  Serial.printf("[sys] верхняя граница измерения %.2f бар, free heap %u\n", sensorMaxBar(),
                ESP.getFreeHeap());

  s_lastPublishMs = millis();
}

void loop() {
  const unsigned long now = millis();

  // Обе функции неблокирующие. Ни одного delay() в рабочем цикле: он бы
  // задерживал веб-портал и приём MQTT.
  portalLoop();
  mqttLoop();

  if (portalConsumeSettingsChanged()) {
    // Настройки изменили с портала — переподключаемся с новыми параметрами.
    mqttApplySettings();
    s_lastPublishMs = now;
  }

  if (now - s_lastSampleMs >= SENSOR_PERIOD_MS) {
    s_lastSampleMs = now;
    sensorSample();
  }

  if (now - s_lastDisplayMs >= DISPLAY_PERIOD_MS) {
    s_lastDisplayMs = now;
    displayReading(sensorRead(), portalWifiConnected(), mqttEnabled(), mqttConnected());
  }

  const unsigned long publishPeriodMs = (unsigned long)settings.mqttIntervalS * 1000UL;
  if (mqttEnabled() && now - s_lastPublishMs >= publishPeriodMs) {
    s_lastPublishMs = now;
    mqttPublishState(sensorRead());
  }
}
