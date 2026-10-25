#include "FirmwareUpload.h"

#include <new>

bool FirmwareUpload::begin(size_t capacity) {
  abort();
  s_error = "";
  if (capacity <= FirmwareImage::HEADER_END + 1) {
    s_error = "Недостаточно места для обновления.";
    return false;
  }
  // Keep one byte unused: even a completely received BIN can never make
  // Updater::isFinished() true before commit. Thus end(false) cannot commit it.
  s_image.begin(capacity - 1);
  s_updater.reset(new (std::nothrow) UpdaterClass());
  if (s_updater) s_updater->runAsync(true);
  if (!s_updater || !s_updater->begin(capacity, U_FLASH)) {
    s_error = "Не удалось начать обновление: недостаточно места или памяти.";
    abort();
    return false;
  }
  return true;
}

bool FirmwareUpload::write(uint8_t *data, size_t length) {
  if (!s_updater) return false;
  if (!s_image.write(data, length)) {
    s_error = s_image.error();
    abort();
    return false;
  }
  if (s_updater->write(data, length) != length) {
    s_error = "Не удалось записать прошивку.";
    abort();
    return false;
  }
  return true;
}

bool FirmwareUpload::finish() {
  if (!s_updater) return false;
  if (!s_image.finish()) {
    s_error = s_image.error();
    abort();
    return false;
  }
  // end(true) is safe only here, after full length and whole-image CRC checks.
  const bool ok = s_updater->end(true);
  if (!ok) s_error = "Updater отклонил образ прошивки.";
  abort();
  return ok;
}

void FirmwareUpload::abort() {
  if (s_updater && s_updater->isRunning()) s_updater->end(false);
  s_updater.reset();
}
