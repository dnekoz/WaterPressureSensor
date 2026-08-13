#pragma once

#include <stddef.h>
#include <stdint.h>

// Чистая логика прошивки: арифметика датчика, валидация настроек и сборка
// MQTT-топиков и payload. Здесь намеренно нет ни одного обращения к железу и
// ни одного включения Arduino.h — благодаря этому модуль собирается и
// тестируется на хосте (см. test/test_logic и окружение [env:native]).

// ---------------------------------------------------------------- настройки ---

struct Settings {
  // --- MQTT ---
  char     mqttHost[64];   // пусто => MQTT выключен, устройство работает автономно
  uint16_t mqttPort;
  char     mqttUser[32];   // пусто => без аутентификации
  char     mqttPass[64];
  char     mqttBase[64];   // базовый топик
  uint16_t mqttIntervalS;  // период публикации состояния
  char     haPrefix[32];   // пусто => не публиковать discovery Home Assistant
  char     deviceName[32]; // имя устройства в Home Assistant

  // --- калибровка ---
  float adcFullScaleV;  // напряжение на пине A0, дающее отсчёт 1023
  float vZero;          // выход датчика при нулевом давлении, В
  float vFull;          // выход датчика при полной шкале, В
  float pFullMPa;       // полная шкала датчика, МПа
};

void settingsDefaults(Settings &s, const char *deviceId);

// Приводит настройки к валидному виду, возвращает true, если что-то поправлено.
// Главное, что здесь гарантируется: vFull - vZero > 0.1, поэтому pressureConvert()
// не может разделить на ноль. haPrefix намеренно не проверяется — пустая строка
// там осмысленна и означает «не публиковать discovery».
bool settingsSanitize(Settings &s, const char *deviceId);

// ----------------------------------------------------------------- измерение ---

// Результат одного пересчёта показаний датчика давления.
struct Reading {
  int   raw;    // усреднённый отсчёт АЦП, 0..1023
  float volts;  // напряжение на пине A0
  float mpa;    // давление, МПа
  float bar;    // давление, бар

  // АЦП упёрся в потолок: давление выше измеримого диапазона и неизвестно.
  bool over;

  // Напряжение ниже физического минимума датчика — обрыв или датчик не подключён.
  bool fault;
};

// Отсчёт, начиная с которого считаем, что АЦП упёрся в потолок.
constexpr int SENSOR_RAW_OVER = 1020;

// Насколько ниже vZero должно упасть напряжение, чтобы счесть датчик оборванным.
// При отключённом A0 плата читает около нуля.
constexpr float SENSOR_FAULT_MARGIN_V = 0.2f;

Reading pressureConvert(int raw, const Settings &s);

// Верхняя граница измеримого давления при текущей калибровке, бар.
float pressureMaxBar(const Settings &s);

// ---------------------------------------------------------------------- MQTT ---

// Собирает "<base>/<suffix>".
void mqttTopic(char *out, size_t size, const Settings &s, const char *suffix);

// Собирает "<haPrefix>/sensor/<deviceId>/<key>/config".
void mqttDiscoveryTopic(char *out, size_t size, const Settings &s, const char *deviceId,
                        const char *key);

// Payload состояния. Возвращает число записанных байт (без завершающего нуля).
size_t mqttStateJson(char *out, size_t size, const Reading &r, int rssi, uint32_t heap);

// Payload discovery для одной сущности Home Assistant.
// devClass и unit могут быть nullptr — тогда соответствующие ключи опускаются.
size_t mqttDiscoveryJson(char *out, size_t size, const Settings &s, const char *deviceId,
                         const char *key, const char *name, const char *devClass,
                         const char *unit, const char *valueTemplate, bool diagnostic);
