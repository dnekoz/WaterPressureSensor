#pragma once

#include <Arduino.h>

#include "Sensor.h"

void mqttBegin();

// Обслуживает соединение: приём пакетов и неблокирующий реконнект с backoff.
// Никогда не ждёт брокера — вызывать каждый проход loop().
void mqttLoop();

// Публикует состояние на <base>/state. Вызывать раз в settings.mqttIntervalS.
void mqttPublishState(const Reading &r);

// Перечитывает настройки: рвёт текущее соединение и подключается заново.
void mqttApplySettings();

// false, если адрес брокера не задан — тогда MQTT просто выключен.
bool mqttEnabled();

bool mqttConnected();
