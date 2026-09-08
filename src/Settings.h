#pragma once

#include "Logic.h"

// Хранение настроек на устройстве. Сама структура Settings и её валидация
// живут в Logic.h — здесь только привязка к железу: LittleFS и chip id.

extern Settings settings;

// Уникальный идентификатор устройства, "wps_a1b2c3". Годен после settingsBegin().
const char *deviceId();

// Монтирует LittleFS и читает /config.json. Форматирует только стёртый раздел.
// Отсутствующие и невалидные поля заполняются значениями по умолчанию.
void settingsBegin();

// Записывает текущие настройки в /config.json. true, если файл записан.
bool settingsSave();
bool settingsSave(const Settings &candidate);
bool settingsCommit(const Settings &candidate);
const char *settingsLastError();
