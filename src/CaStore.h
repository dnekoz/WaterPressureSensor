#pragma once

#include <Arduino.h>
#include <BearSSLHelpers.h>

// Хранение корневого сертификата (CA) для режима TLS_CA: файл /ca.pem на
// LittleFS плюс его разбор в X509List. Сертификат загружается с портала и
// переживает перепрошивку — иначе смена CA у запаянного в щиток устройства
// означала бы разборку и USB.

bool   caExists();
size_t caSize();
bool   caRemove();

// Приём файла по кускам — именно так его отдаёт обработчик загрузки
// ESP8266WebServer. Пишется во временный файл; /ca.pem заменяется только в
// caWriteEnd() и только если содержимое разобралось как сертификат.
bool caWriteBegin();
bool caWriteChunk(const uint8_t *data, size_t len);
bool caWriteEnd();
void caWriteAbort();

// Разбирает /ca.pem. nullptr, если файла нет или это не сертификат.
// Владение переходит вызывающему.
BearSSL::X509List *caLoad();

// Почему последняя загрузка или разбор не удались. Пустая строка, если всё
// хорошо. Показывается на странице портала: иначе «TLS не подключается»
// неотличимо от неверного пароля.
const char *caLastError();
