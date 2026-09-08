#include "Sensor.h"

#include <Arduino.h>

#include "Config.h"
#include "Settings.h"

static PressureFilter filter;

void sensorBegin() {
  filter = PressureFilter();
  sensorSample();
}

void sensorSample() {
  pressureFilterSample(filter, analogRead(SENSOR_PIN), millis(), settings);
}

Reading sensorRead() {
  return pressureFilterRead(filter, millis(), settings);
}

float sensorMaxBar() { return pressureMaxBar(settings); }
