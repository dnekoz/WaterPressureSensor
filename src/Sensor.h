#pragma once

#include "Logic.h"

// Съём показаний с A0. Пересчёт отсчёта в давление живёт в Logic.h
// (pressureConvert) — здесь только работа с железом и усреднение.

void sensorBegin();

// Берёт один отсчёт в кольцевой буфер. Вызывать раз в SENSOR_PERIOD_MS.
void sensorSample();

// Пересчитывает усреднённый буфер по текущей калибровке из settings.
Reading sensorRead();

// Верхняя граница измеримого давления при текущей калибровке, бар.
float sensorMaxBar();
