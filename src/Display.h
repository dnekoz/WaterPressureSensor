#pragma once

#include <Arduino.h>

#include "Sensor.h"

// Инициализирует OLED. false, если дисплей не отвечает по I2C — в этом случае
// остальные функции модуля становятся пустышками, а прошивка продолжает
// работать «вслепую» (измерения и MQTT не зависят от экрана).
bool displayBegin();

// Заставка при старте: имя прошивки и версия.
void displayBoot();

// Режим настройки: имя точки доступа, пароль и адрес портала.
void displayPortal(const char *apSsid, const char *apPass, const char *ip);

// Временный экран с паролем для открытого окна входа в веб-интерфейсе.
void displayLoginPassword(const char *password);

// Промежуточное состояние с одной-двумя строками текста.
void displayMessage(const char *line1, const char *line2 = nullptr);

// Рабочий экран: крупное давление плюс строка состояния связи.
void displayReading(const Reading &r, bool wifiOk, bool mqttEnabled, bool mqttOk, bool tls);
