#pragma once

#include <Updater.h>
#include <memory>
#include "FirmwareImage.h"

// Owns one uncommitted OTA transaction. UpdaterClass 3.1.2's destructor does
// not free its raw buffer, so every unsuccessful exit must explicitly cancel.
class FirmwareUpload {
public:
  ~FirmwareUpload() { abort(); }
  bool begin(size_t capacity);
  bool write(uint8_t *data, size_t length);
  bool finish();
  void abort();
  const char *error() const { return s_error; }

private:
  FirmwareImage s_image;
  std::unique_ptr<UpdaterClass> s_updater;
  const char *s_error = "";
};
