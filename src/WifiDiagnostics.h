#pragma once

#if defined(WPS_WIFI_DIAGNOSTICS)

// Temporary, observational ESP8266 Wi-Fi diagnostics. LittleFS must already be
// mounted. These functions never mount/format it or change Wi-Fi configuration.
void wifiDiagnosticsBegin();
void wifiDiagnosticsLoop();

// Main context only: commit a pre-call marker immediately before the existing
// reconnect(), so a stuck SDK call does not erase its starting state.
void wifiDiagnosticsBeforeRetry();

// Call once AFTER the existing reconnect() call, passing its original result.
// This only queues a record; all flash operations happen in Begin/Loop.
void wifiDiagnosticsRetry(bool result);

bool wifiDiagnosticsEnabled();
const char *wifiDiagnosticsError();
const char *wifiDiagnosticsLogPath();
const char *wifiDiagnosticsPreviousLogPath();

#else

// The normal firmware keeps no diagnostic state, handlers or filesystem code.
inline void wifiDiagnosticsBegin() {}
inline void wifiDiagnosticsLoop() {}
inline void wifiDiagnosticsBeforeRetry() {}
inline void wifiDiagnosticsRetry(bool) {}

#endif
