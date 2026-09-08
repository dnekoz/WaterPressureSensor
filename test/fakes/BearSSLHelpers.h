#pragma once
#include "LittleFS.h"
// Only tests the file replacement transaction, not cryptographic PEM parsing.
namespace BearSSL {
class X509List {
  bool valid = false;
public:
  X509List(File &file, size_t length) {
    std::string pem(length, '\0');
    file.readBytes(&pem[0], length);
    valid = pem.find("BEGIN CERTIFICATE") != std::string::npos && pem.find("END CERTIFICATE") != std::string::npos;
  }
  size_t getCount() const { return valid ? 1 : 0; }
};
}
