#pragma once
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <limits>
#include "Arduino.h"

struct Faults {
  size_t limit = std::numeric_limits<size_t>::max();
  bool rename = false;
  bool corrupt = false;
  bool truncateOnClose = false;
};
static Faults faults;
struct LittleFSConfig { explicit LittleFSConfig(bool) {} };
class File {
  std::shared_ptr<std::string> data;
  size_t position = 0;
  bool writable = false;
public:
  File() = default;
  File(std::shared_ptr<std::string> text, bool writing): data(text), writable(writing) {}
  explicit operator bool() const { return bool(data); }
  size_t size() const { return data ? data->size() : 0; }
  int read() { return data && position < data->size() ? (uint8_t)(*data)[position++] : -1; }
  size_t readBytes(char *out, size_t length) {
    size_t count = 0;
    while (count < length) { int next = read(); if (next < 0) break; out[count++] = char(next); }
    return count;
  }
  bool seek(size_t offset) { if (!data || offset > data->size()) return false; position = offset; return true; }
  size_t write(uint8_t byte) { return write(&byte, 1); }
  size_t write(const uint8_t *bytes, size_t length) {
    if (!data || !writable || data->size() >= faults.limit) return 0;
    size_t written = std::min(length, faults.limit - data->size());
    data->append((const char *)bytes, written);
    return written;
  }
  void flush() {}
  void close() {
    if (data && writable && faults.corrupt && data->size() > 10) (*data)[10] = '?';
    if (data && writable && faults.truncateOnClose && !data->empty()) data->pop_back();
    data.reset();
  }
};
struct FakeFs {
  std::map<std::string, std::shared_ptr<std::string>> files;
  bool begin() { return true; }
  void setConfig(LittleFSConfig) {}
  bool format() { files.clear(); return true; }
  bool exists(const char *path) { return files.count(path); }
  File open(const char *path, const char *mode) {
    bool write = mode[0] == 'w';
    if (write) files[path] = std::make_shared<std::string>();
    auto found = files.find(path);
    if (found == files.end()) return File();
    return File(found->second, write);
  }
  bool remove(const char *path) { return files.erase(path); }
  bool rename(const char *from, const char *to) {
    if (faults.rename || !exists(from)) return false;
    files[to] = files[from]; files.erase(from); return true;
  }
  std::string contents(const char *path) { return exists(path) ? *files[path] : ""; }
};
static FakeFs LittleFS;
