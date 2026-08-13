#pragma once

// Общие константы прошивки. Всё, что настраивается на портале, живёт в Settings.h;
// здесь только то, что меняется перепрошивкой.

#define FW_NAME    "WaterPressureSensor"
#define FW_VERSION "2.0.0"

// ---------------------------------------------------------------- дисплей ---

#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64
#define SCREEN_ADDRESS 0x3C  // 0x3C у большинства модулей 128x64, реже 0x3D

// -1 = у модуля нет вывода RESET (обычный случай для I2C-плат SSD1306).
// В прежней версии тут стоял 0, то есть GPIO0 — boot/flash-пин D1 mini.
#define OLED_RESET -1

// --------------------------------------------------------------- измерение ---

#define SENSOR_PIN        A0
#define SENSOR_SAMPLES    32  // окно скользящего среднего
#define SENSOR_PERIOD_MS  20  // период выборки => окно ~0.64 с

// Пороги SENSOR_RAW_OVER и SENSOR_FAULT_MARGIN_V живут в Logic.h: они часть
// чистой логики пересчёта и покрыты тестами.

// ------------------------------------------------------------------ портал ---

// Пароль точки доступа, которую поднимает устройство в режиме настройки.
// Минимум 8 символов — иначе WPA2 не поднимется и AP будет открытой.
#define PORTAL_AP_PASSWORD "pressure123"

// Сколько ждать в портале, прежде чем продолжить работу офлайн.
#define PORTAL_TIMEOUT_S 180

// Сколько минут без WiFi терпеть до перезагрузки. 0 = не перезагружаться.
#define WIFI_LOST_RESTART_MIN 15

// -------------------------------------------------------------------- MQTT ---

// PubSubClient по умолчанию даёт 256 байт — в них не влезает discovery-payload
// Home Assistant, и publish() молча вернёт false.
#define MQTT_BUFFER_SIZE 1024

#define MQTT_RECONNECT_MIN_MS 5000UL
#define MQTT_RECONNECT_MAX_MS 30000UL

// ------------------------------------------------------------- планировщик ---

#define DISPLAY_PERIOD_MS 250
