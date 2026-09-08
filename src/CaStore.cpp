#include "CaStore.h"

#include <LittleFS.h>

#include "Config.h"

static const char *CA_PATH     = "/ca.pem";
static const char *CA_TMP_PATH = "/ca.tmp";

static File s_upload;
static size_t s_uploaded = 0;
static char   s_error[160] = "";

static void setError(const char *msg) {
  strncpy(s_error, msg, sizeof(s_error) - 1);
  s_error[sizeof(s_error) - 1] = '\0';
}

const char *caLastError() { return s_error; }

bool caExists() { return LittleFS.exists(CA_PATH); }

size_t caSize() {
  File f = LittleFS.open(CA_PATH, "r");
  if (!f) return 0;
  size_t n = f.size();
  f.close();
  return n;
}

bool caRemove() {
  s_error[0] = '\0';
  if (!caExists() || LittleFS.remove(CA_PATH)) return true;
  setError("Не удалось удалить сертификат.");
  return false;
}

// Разбирает PEM прямо из файла: X509List умеет читать из Stream, и промежуточный
// буфер на пару килобайт в куче не нужен — она здесь и так узкое место.
static BearSSL::X509List *parseFile(const char *path) {
  File f = LittleFS.open(path, "r");
  if (!f) return nullptr;
  if (!f.size() || f.size() > CA_MAX_SIZE) { f.close(); return nullptr; }

  BearSSL::X509List *list = new BearSSL::X509List(f, f.size());
  f.close();

  if (list && list->getCount() > 0) return list;

  delete list;
  return nullptr;
}

bool caWriteBegin() {
  caWriteAbort();
  s_error[0] = '\0';
  s_uploaded = 0;

  s_upload = LittleFS.open(CA_TMP_PATH, "w");
  if (!s_upload) {
    setError("не удалось открыть файл на запись");
    return false;
  }
  return true;
}

bool caWriteChunk(const uint8_t *data, size_t len) {
  if (!s_upload) return false;

  // Ограничение и от опечатки (выбрали не тот файл), и от переполнения LittleFS:
  // разумный CA — пара килобайт.
  if (len > CA_MAX_SIZE - s_uploaded) {
    setError("файл слишком большой");
    caWriteAbort();
    return false;
  }

  if (s_upload.write(data, len) != len) {
    setError("запись оборвалась, кончилось место?");
    caWriteAbort();
    return false;
  }

  s_uploaded += len;
  return true;
}

void caWriteAbort() {
  if (s_upload) s_upload.close();
  LittleFS.remove(CA_TMP_PATH);
  s_uploaded = 0;
}

bool caWriteEnd() {
  if (!s_upload) {
    if (s_error[0] == '\0') setError("загрузка не начиналась");
    return false;
  }

  s_upload.flush();
  s_upload.close();

  if (s_uploaded == 0) {
    setError("пустой файл");
    LittleFS.remove(CA_TMP_PATH);
    return false;
  }

  File stored = LittleFS.open(CA_TMP_PATH, "r");
  bool complete = stored && stored.size() == s_uploaded;
  if (stored) stored.close();
  if (!complete) {
    setError("Запись сертификата неполная. Прежний файл сохранён.");
    LittleFS.remove(CA_TMP_PATH);
    return false;
  }

  // Проверяем до замены рабочего файла: иначе неверный PEM молча заменил бы
  // рабочий сертификат, и устройство отвалилось бы от брокера уже после
  // «успешной» загрузки.
  BearSSL::X509List *check = parseFile(CA_TMP_PATH);
  if (!check) {
    setError("это не сертификат в формате PEM");
    LittleFS.remove(CA_TMP_PATH);
    return false;
  }
  delete check;

  // Атомарная замена: сбой оставляет предыдущий сертификат на месте.
  if (!LittleFS.rename(CA_TMP_PATH, CA_PATH)) {
    setError("не удалось переименовать файл");
    LittleFS.remove(CA_TMP_PATH);
    return false;
  }

  s_error[0] = '\0';
  Serial.printf("[ca] сертификат сохранён, %u байт\n", (unsigned)s_uploaded);
  return true;
}

BearSSL::X509List *caLoad() {
  if (!caExists()) {
    setError("сертификат не загружен");
    return nullptr;
  }

  BearSSL::X509List *list = parseFile(CA_PATH);
  if (!list) {
    setError("сохранённый сертификат не разбирается");
    return nullptr;
  }

  s_error[0] = '\0';
  return list;
}
