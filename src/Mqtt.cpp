#include "Mqtt.h"

#include <ESP8266WiFi.h>
#include <PubSubClient.h>

#include "Config.h"
#include "Logic.h"
#include "Settings.h"

static WiFiClient   net;
static PubSubClient client(net);

static unsigned long s_lastAttemptMs = 0;
static unsigned long s_backoffMs     = MQTT_RECONNECT_MIN_MS;
static bool          s_everAttempted = false;

bool mqttEnabled() { return settings.mqttHost[0] != '\0'; }

bool mqttConnected() { return client.connected(); }

void mqttBegin() {
  // У PubSubClient 2.8 буфер по умолчанию 256 байт. Discovery-payload Home
  // Assistant в него не помещается, и publish() молча возвращает false —
  // именно так эта связка обычно и «не работает без ошибок».
  if (!client.setBufferSize(MQTT_BUFFER_SIZE)) {
    Serial.println(F("[mqtt] не хватило памяти на буфер, discovery может не пройти"));
  }
  client.setKeepAlive(30);
  mqttApplySettings();
}

void mqttApplySettings() {
  if (client.connected()) client.disconnect();

  s_backoffMs     = MQTT_RECONNECT_MIN_MS;
  s_lastAttemptMs = 0;
  s_everAttempted = false;

  if (mqttEnabled()) {
    client.setServer(settings.mqttHost, settings.mqttPort);
    Serial.printf("[mqtt] брокер %s:%u, топик %s\n", settings.mqttHost, settings.mqttPort,
                  settings.mqttBase);
  } else {
    Serial.println(F("[mqtt] адрес брокера не задан, MQTT выключен"));
  }
}

// Одна сущность Home Assistant.
static void publishDiscovery(const char *key, const char *name, const char *devClass,
                             const char *unit, const char *valueTemplate, bool diagnostic) {
  char cfgTopic[160];
  mqttDiscoveryTopic(cfgTopic, sizeof(cfgTopic), settings, deviceId(), key);

  // Статический, а не стековый: у ESP8266 стек рабочего контекста всего ~4 КБ,
  // а функция не реентерабельна.
  static char payload[MQTT_BUFFER_SIZE];
  size_t len = mqttDiscoveryJson(payload, sizeof(payload), settings, deviceId(), key, name,
                                 devClass, unit, valueTemplate, diagnostic);

  if (!client.publish(cfgTopic, payload, true)) {
    Serial.printf("[mqtt] discovery %s не отправлен (%u байт), увеличьте MQTT_BUFFER_SIZE\n", key,
                  (unsigned)len);
  }
}

static void publishDiscoveryAll() {
  // Пустой префикс — штатный способ отключить интеграцию с Home Assistant.
  if (settings.haPrefix[0] == '\0') return;

  publishDiscovery("pressure", "Pressure", "pressure", "bar", "{{ value_json.bar }}", false);
  publishDiscovery("voltage", "Sensor voltage", "voltage", "V", "{{ value_json.volt }}", false);
  publishDiscovery("rssi", "WiFi signal", "signal_strength", "dBm", "{{ value_json.rssi }}", true);
  publishDiscovery("raw", "ADC raw", nullptr, nullptr, "{{ value_json.raw }}", true);
  Serial.println(F("[mqtt] discovery для Home Assistant опубликован"));
}

static void tryConnect() {
  char availTopic[96];
  mqttTopic(availTopic, sizeof(availTopic), settings, "status");

  // Пустые логин/пароль надо передавать как NULL: пустая строка означала бы
  // попытку аутентификации с пустым пользователем, и брокер её отклонит.
  const char *user = settings.mqttUser[0] ? settings.mqttUser : nullptr;
  const char *pass = settings.mqttPass[0] ? settings.mqttPass : nullptr;

  // Последняя воля: если устройство пропадёт, брокер сам разошлёт "offline".
  bool ok = client.connect(deviceId(), user, pass, availTopic, 0, true, "offline");

  if (!ok) {
    Serial.printf("[mqtt] подключиться не удалось, state=%d, повтор через %lu с\n", client.state(),
                  s_backoffMs / 1000);
    s_backoffMs = min(s_backoffMs * 2, (unsigned long)MQTT_RECONNECT_MAX_MS);
    return;
  }

  Serial.println(F("[mqtt] подключено"));
  s_backoffMs = MQTT_RECONNECT_MIN_MS;

  client.publish(availTopic, "online", true);
  publishDiscoveryAll();
}

void mqttLoop() {
  if (!mqttEnabled() || WiFi.status() != WL_CONNECTED) return;

  if (client.connected()) {
    client.loop();
    return;
  }

  // Первая попытка — сразу, дальше по backoff. Вычитание unsigned long
  // корректно переживает переполнение millis().
  if (s_everAttempted && (millis() - s_lastAttemptMs) < s_backoffMs) return;

  s_everAttempted = true;
  s_lastAttemptMs = millis();
  tryConnect();
}

void mqttPublishState(const Reading &r) {
  if (!client.connected()) return;

  char stateTopic[96];
  mqttTopic(stateTopic, sizeof(stateTopic), settings, "state");

  char payload[256];
  mqttStateJson(payload, sizeof(payload), r, WiFi.RSSI(), ESP.getFreeHeap());

  if (!client.publish(stateTopic, payload, false)) {
    Serial.println(F("[mqtt] состояние отправить не удалось"));
  }
}
