#include "Sensor.h"

#include <Arduino.h>

#include "Config.h"
#include "Settings.h"

static int     s_buf[SENSOR_SAMPLES];
static uint8_t s_idx    = 0;
static long    s_total  = 0;
static bool    s_primed = false;  // буфер ещё не заполнен целиком

void sensorBegin() {
  // Заполняем буфер текущим значением, а не нулями: иначе первые ~0.6 с
  // на экране будет уезжающее вверх фальшивое давление.
  int first = analogRead(SENSOR_PIN);
  for (uint8_t i = 0; i < SENSOR_SAMPLES; i++) s_buf[i] = first;
  s_total  = (long)first * SENSOR_SAMPLES;
  s_idx    = 0;
  s_primed = true;
}

void sensorSample() {
  int v = analogRead(SENSOR_PIN);
  s_total -= s_buf[s_idx];
  s_buf[s_idx] = v;
  s_total += v;
  s_idx = (s_idx + 1) % SENSOR_SAMPLES;
}

Reading sensorRead() {
  int raw = s_primed ? (int)(s_total / SENSOR_SAMPLES) : analogRead(SENSOR_PIN);
  return pressureConvert(raw, settings);
}

float sensorMaxBar() { return pressureMaxBar(settings); }
