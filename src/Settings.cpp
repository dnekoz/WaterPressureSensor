#include "Settings.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#ifdef ARDUINO_ARCH_ESP8266
#include <flash_hal.h>
#endif

#include "Config.h"

// Здесь только привязка к железу: LittleFS и chip id. Сам список полей и его
// сериализация живут в Logic.cpp, где их покрывает round-trip тест.

static const char *CONFIG_PATH = "/config.json";

// Настройки пишутся сюда и переименовываются поверх CONFIG_PATH только после
// успешной записи. Иначе пропадание питания посреди serializeJson() оставляло бы
// обрезанный JSON, а он при следующем старте не разбирается — и MQTT молча
// выключался бы, откатившись к значениям по умолчанию.
static const char *CONFIG_TMP_PATH = "/config.tmp";

Settings settings;

static char s_deviceId[16];
static char s_error[160] = "";
const char *settingsLastError() { return s_error; }

static bool saveError(const char *message) {
  snprintf(s_error, sizeof(s_error), "%s", message);
  Serial.printf("[cfg] %s\n", message);
  return false;
}

static void ensurePortalPassword() {
  if (settingsEnsurePortalPassword(settings)) settingsSave();
}

const char *deviceId() { return s_deviceId; }

static bool filesystemIsBlank() {
#ifdef ARDUINO_ARCH_ESP8266
  // Форматирование разрешено автоматически только для стёртого раздела.
  // Ошибка монтирования существующей FS не должна уничтожать CA и настройки.
  uint32_t block[64];
  for (uint32_t offset = 0; offset < FS_PHYS_SIZE; offset += sizeof(block)) {
    if (!ESP.flashRead(FS_PHYS_ADDR + offset, block, sizeof(block))) return false;
    for (uint32_t word : block) if (word != 0xffffffffUL) return false;
    yield();
  }
  return FS_PHYS_SIZE > 0;
#else
  return false; // hardware flash scanning is outside native persistence tests
#endif
}

void settingsBegin() {
  snprintf(s_deviceId, sizeof(s_deviceId), "wps_%06x", ESP.getChipId());

  settingsDefaults(settings, s_deviceId);

  LittleFS.setConfig(LittleFSConfig(false));
  if (!LittleFS.begin()) {
    if (!filesystemIsBlank() || !LittleFS.format() || !LittleFS.begin()) {
      ensurePortalPassword();
      saveError("LittleFS недоступна. Раздел не стёрт; настройки сейчас только в памяти.");
      return;
    }
  }

  // Сначала штатный файл, затем config.tmp для восстановления старых версий,
  // которые удаляли config.json перед переименованием временного файла.
  JsonDocument doc;
  const char  *sources[] = {CONFIG_PATH, CONFIG_TMP_PATH};
  int          usedIdx   = -1;

  for (int i = 0; i < 2; i++) {
    File f = LittleFS.open(sources[i], "r");
    if (!f) continue;
    if (!f.size() || f.size() > 4096) { f.close(); continue; }

    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err || !doc.is<JsonObject>()) {
      Serial.printf("[cfg] %s повреждён (%s)\n", sources[i], err.c_str());
      continue;
    }

    usedIdx = i;
    break;
  }

  if (usedIdx < 0) {
    Serial.println(F("[cfg] настроек нет, беру значения по умолчанию"));
    settingsDefaults(settings, s_deviceId);
    LittleFS.remove(CONFIG_TMP_PATH);
    ensurePortalPassword();
    return;
  }

  settingsFromJson(doc.as<JsonObjectConst>(), settings);

  if (settingsSanitize(settings, s_deviceId)) {
    Serial.println(F("[cfg] в настройках были недопустимые значения, часть полей сброшена"));
  }

  if (usedIdx == 1) {
    // Сохраняем восстановленный файл без повторной сериализации и удаления.
    Serial.println(F("[cfg] восстановлено из config.tmp, дописываю config.json"));
    if (!LittleFS.rename(CONFIG_TMP_PATH, CONFIG_PATH))
      saveError("Не удалось восстановить config.json из временного файла.");
  } else {
    LittleFS.remove(CONFIG_TMP_PATH);
    Serial.println(F("[cfg] настройки загружены"));
  }
  ensurePortalPassword();
}

bool settingsSave() {
  return settingsSave(settings);
}

bool settingsCommit(const Settings &candidate) {
  if (!settingsSave(candidate)) return false;
  settings = candidate;
  return true;
}

bool settingsSave(const Settings &candidate) {
  s_error[0] = '\0';
  JsonDocument doc;
  settingsToJson(doc.to<JsonObject>(), candidate);
  if (doc.overflowed()) return saveError("Недостаточно памяти для сохранения настроек.");
  const size_t expected = measureJson(doc);

  File f = LittleFS.open(CONFIG_TMP_PATH, "w");
  if (!f) {
    return saveError("Не удалось открыть временный файл. Настройки не сохранены.");
  }

  size_t written = serializeJson(doc, f);
  f.flush();
  f.close();

  // Перечитываем размер: serializeJson() сообщает, сколько байт отдал, но при
  // кончившемся месте на LittleFS до флеша доедет меньше, и молча.
  File   check  = LittleFS.open(CONFIG_TMP_PATH, "r");
  size_t onDisk = check ? check.size() : 0;
  JsonDocument verified;
  bool valid = check && !deserializeJson(verified, check) && verified.is<JsonObject>();
  // Сравниваем именно записанные байты: float после JSON round-trip может
  // отличаться от double в DOM, хотя файл абсолютно корректен.
  struct Comparator {
    File &file;
    bool matches = true;
    size_t write(uint8_t byte) { if (file.read() != byte) matches = false; return 1; }
    size_t write(const uint8_t *data, size_t size) {
      for (size_t i = 0; i < size; ++i) write(data[i]);
      return size;
    }
  } comparator{check};
  if (valid) {
    valid = check.seek(0) && serializeJson(doc, comparator) == expected && comparator.matches;
  }
  if (check) check.close();

  if (!expected || written != expected || onDisk != expected || !valid) {
    // Старый config.json цел — устройство продолжит работать на прежних настройках.
    Serial.printf("[cfg] запись не удалась (%u из %u байт), настройки не изменены\n",
                  (unsigned)onDisk, (unsigned)written);
    LittleFS.remove(CONFIG_TMP_PATH);
    return saveError("Запись настроек неполная или повреждена. Прежний файл сохранён.");
  }

  // LittleFS атомарно заменяет существующий файл. Не удаляем рабочую копию.
  if (!LittleFS.rename(CONFIG_TMP_PATH, CONFIG_PATH)) {
    return saveError("Не удалось заменить файл настроек. Прежний файл сохранён.");
  }

  Serial.println(F("[cfg] настройки сохранены"));
  return true;
}
