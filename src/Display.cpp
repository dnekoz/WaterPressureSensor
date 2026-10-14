#include "Display.h"

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Wire.h>

#include "Config.h"

static Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
static bool s_ready = false;

// У шрифтов Adafruit GFX setCursor() задаёт базовую линию, а не верхний левый
// угол. Отсюда координаты ниже: строка size 2 ростом ~26 px с базовой линией 28
// занимает верхнюю половину экрана, дальше две обычные строки.
static const int16_t BASELINE_BIG    = 30;
static const int16_t BASELINE_MIDDLE = 48;
static const int16_t BASELINE_STATUS = 63;

bool displayBegin() {
  Wire.begin();
  Wire.setClock(400000);

  Wire.beginTransmission(SCREEN_ADDRESS);
  if (Wire.endTransmission() != 0 || !oled.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    // Прежняя версия здесь уходила в вечный for(;;). Дисплей — не критичный
    // узел: без него устройство всё равно меряет и публикует.
    Serial.println(F("[oled] дисплей не найден, работаю без экрана"));
    s_ready = false;
    return false;
  }

  oled.clearDisplay();
  oled.display();
  oled.setFont(&FreeSans9pt7b);
  oled.setTextColor(SSD1306_WHITE);
  s_ready = true;
  return true;
}

// Ширина строки текущим шрифтом и размером.
static int16_t textWidth(const char *s) {
  int16_t  x1, y1;
  uint16_t w, h;
  oled.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return (int16_t)w;
}

void displayBoot() {
  if (!s_ready) return;

  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setCursor(0, 26);
  oled.print(F("Water Pressure"));
  oled.setCursor(0, 46);
  oled.print(F("v" FW_VERSION));
  oled.display();
}

void displayPortal(const char *apSsid, const char *apPass, const char *ip) {
  if (!s_ready) return;

  oled.clearDisplay();
  oled.setFont(nullptr);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print(F("SETUP MODE"));
  oled.setCursor(0, 12);
  oled.print(apSsid);
  oled.setCursor(0, 24);
  oled.print(apPass);
  oled.setCursor(0, 56);
  oled.print(ip);
  oled.display();
  oled.setFont(&FreeSans9pt7b);
}

void displayLoginPassword(const char *password) {
  if (!s_ready) return;

  oled.clearDisplay();
  oled.setFont(nullptr);
  oled.setTextWrap(false);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print(F("SETTINGS PASSWORD"));

  const size_t length = strlen(password);
  if (length <= 10) {
    // Стандартный пароль читается крупно: 10 символов по 12 пикселей.
    oled.setTextSize(2);
    oled.setCursor((SCREEN_WIDTH - length * 12) / 2, 24);
    oled.print(password);
  } else {
    // Допустимые 63 символа помещаются в три строки по 21 символу.
    char line[22];
    for (size_t offset = 0; offset < length && offset < 63; offset += 21) {
      const size_t count = min(size_t(21), length - offset);
      memcpy(line, password + offset, count);
      line[count] = '\0';
      oled.setCursor(0, 18 + (offset / 21) * 10);
      oled.print(line);
    }
  }
  oled.display();
  oled.setTextWrap(true);
  oled.setFont(&FreeSans9pt7b);
}

void displayMessage(const char *line1, const char *line2) {
  if (!s_ready) return;

  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setCursor(0, 28);
  oled.print(line1);
  if (line2) {
    oled.setCursor(0, 50);
    oled.print(line2);
  }
  oled.display();
}

// Нижняя строка: состояние WiFi и MQTT. Подключение по TLS обозначается
// отдельно — без этого невозможно на месте убедиться, что шифрование включилось,
// а не тихо откатилось.
static void drawStatus(bool wifiOk, bool mqttEnabled, bool mqttOk, bool tls) {
  oled.setTextSize(1);
  oled.setCursor(0, BASELINE_STATUS);
  oled.print(wifiOk ? F("W:ok") : F("W:--"));
  oled.setCursor(56, BASELINE_STATUS);
  if (!mqttEnabled) {
    oled.print(F("M:off"));
  } else if (mqttOk) {
    oled.print(tls ? F("M:tls") : F("M:ok"));
  } else {
    oled.print(F("M:--"));
  }
}

void displayReading(const Reading &r, bool wifiOk, bool mqttEnabled, bool mqttOk, bool tls) {
  if (!s_ready) return;

  oled.clearDisplay();

  if (r.stale) {
    oled.setTextSize(1);
    oled.setCursor(0, 20);
    oled.print(F("STALE DATA"));
    oled.setCursor(0, 42);
    oled.print(F("waiting..."));
  } else if (r.fault) {
    oled.setTextSize(1);
    oled.setCursor(0, 20);
    oled.print(F("NO SENSOR"));
    oled.setCursor(0, 42);
    oled.print(F("check A0"));
  } else if (r.over) {
    oled.setTextSize(2);
    oled.setCursor(0, BASELINE_BIG);
    oled.print(F("OVER"));
    oled.setTextSize(1);
    oled.setCursor(0, BASELINE_MIDDLE);
    oled.printf(">%.2f bar", sensorMaxBar());
  } else {
    char num[16];
    snprintf(num, sizeof(num), "%.2f", r.bar);

    // Крупное число, следом единица обычным размером — целиком «2.47 bar»
    // размером 2 в 128 px не помещается.
    oled.setTextSize(2);
    oled.setCursor(0, BASELINE_BIG);
    oled.print(num);
    int16_t numW = textWidth(num);

    oled.setTextSize(1);
    oled.setCursor(numW + 6, BASELINE_BIG);
    oled.print(F("bar"));

    oled.setCursor(0, BASELINE_MIDDLE);
    oled.printf("%.3f MPa", r.mpa);
  }

  drawStatus(wifiOk, mqttEnabled, mqttOk, tls);
  oled.display();
}
