#include <Arduino.h>

#include <time.h>

#include "Config.h"
#include "Display.h"
#include "Mqtt.h"
#include "Portal.h"
#include "Sensor.h"
#include "Settings.h"

static unsigned long s_lastSampleMs  = 0;
static unsigned long s_lastDisplayMs = 0;
static unsigned long s_lastPublishMs = 0;

// Часы нужны режиму TLS_CA: без них проверка срока действия сертификата
// провалится. Смещения нулевые — сравнивается только UTC, местное время
// устройству ни для чего не нужно.
static void startTimeSync() {
  if (settings.mqttTlsMode != TLS_CA) return;

  const char *ntp = settings.ntpServer[0] ? settings.ntpServer : NTP_SERVER_DEFAULT;
  configTime(0, 0, ntp);
  Serial.printf("[time] синхронизация по %s\n", ntp);
}

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
  startTimeSync();
  mqttBegin();

  // Максимальный свободный блок важнее суммы: рукопожатию TLS нужен непрерывный
  // приёмный буфер, и при фрагментированной куче оно откажет при живых 30 КБ.
  Serial.printf("[sys] верхняя граница измерения %.2f бар, free heap %u, макс. блок %u\n",
                sensorMaxBar(), ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());

  s_lastPublishMs = millis();
}

void loop() {
  const unsigned long before = millis();

  // Ни одного delay() в рабочем цикле: он задерживал бы веб-портал и приём MQTT.
  // Полностью неблокирующими эти две функции всё же не сделать — попытка
  // подключения к брокеру и рукопожатие TLS блокируют по своей природе.
  portalLoop();

  if (portalConsumeSettingsChanged()) {
    // Настройки изменили с портала — переподключаемся с новыми параметрами.
    // Порядок важен: NTP может понадобиться режиму, который только что включили.
    startTimeSync();
    mqttApplySettings();
    sensorBegin();
    s_lastPublishMs = millis();
  }

  mqttLoop();
  const unsigned long now = millis();

  if (now - before >= SCHEDULER_STALL_MS || now - s_lastSampleMs >= SENSOR_PERIOD_MS) {
    s_lastSampleMs = now;
    sensorSample();
  }

  if (mqttConsumeJustConnected()) {
    sensorSample();
    s_lastSampleMs = now;
    mqttPublishState(sensorRead());
    s_lastPublishMs = now;
  }

  if (now - s_lastDisplayMs >= DISPLAY_PERIOD_MS) {
    s_lastDisplayMs = now;
    if (!portalDrawIfActive()) {
      displayReading(sensorRead(), portalWifiConnected(), mqttEnabled(), mqttConnected(),
                     mqttTlsActive());
    }
  }

  const unsigned long publishPeriodMs = (unsigned long)settings.mqttIntervalS * 1000UL;
  if (mqttEnabled() && now - s_lastPublishMs >= publishPeriodMs) {
    s_lastPublishMs = now;
    mqttPublishState(sensorRead());
  }
}
