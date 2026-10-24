#include "WifiDiagnostics.h"

#if defined(WPS_WIFI_DIAGNOSTICS)

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <interrupts.h>
#include <stdarg.h>
#include <time.h>
extern "C" {
#include <user_interface.h>
}

#include "Config.h"
#include "Settings.h"

namespace {
constexpr char LOG_PATH[] = "/wifi-diagnostic.log";
constexpr char PREVIOUS_PATH[] = "/wifi-diagnostic.prev.log";
constexpr size_t LOG_LIMIT = 16384;
constexpr size_t FS_RESERVE = 8192;
constexpr uint8_t QUEUE_SIZE = 24;
constexpr uint8_t DRAIN_LIMIT = 8;
constexpr uint32_t HEARTBEAT_MS = 60000;

enum class Kind : uint8_t {
  Boot, Connected, Disconnected, GotIp, DhcpTimeout, Mode, AuthMode, RetryBegin, Retry,
  State, Heartbeat, Overflow
};

struct Event {
  uint32_t ms;
  uint32_t ip;
  uint32_t mask;
  uint32_t gateway;
  uint8_t bssid[6];
  Kind kind;
  uint8_t reason;
  uint8_t channel;
  uint8_t oldMode;
  uint8_t newMode;
  bool result;
};
static_assert(sizeof(Event) <= 28, "Keep the callback queue small");

struct Snapshot {
  uint32_t ms;
  uint32_t ip;
  uint32_t heap;
  uint32_t block;
  int32_t rssi;
  uint8_t sdk;
  uint8_t status;
  uint8_t mode;
  uint8_t dhcp;
  uint8_t channel;
  bool autoReconnect;
  bool autoConnect;
  int8_t configBssidSet;
  uint8_t configBssid[6];
};

Event s_queue[QUEUE_SIZE];
volatile uint8_t s_head = 0;
volatile uint8_t s_tail = 0;
volatile uint8_t s_count = 0;
volatile uint32_t s_dropped = 0;
volatile bool s_enabled = false;
bool s_started = false;
char s_error[80] = "";
char s_line[768];
uint32_t s_boot = 0;
uint32_t s_sequence = 0;
uint32_t s_heartbeatMs = 0;
uint32_t s_lastLoopMs = 0;
uint32_t s_maxLoopGap = 0;
uint32_t s_reportedDropped = 0;
uint32_t s_firstReasonMs = 0;
uint8_t s_firstReason = 0;
uint8_t s_lastReason = 0;
bool s_inOutage = false;
Snapshot s_previous{};

// Retain these for the whole process lifetime: the core removes handlers when
// their last user-owned shared_ptr expires.
WiFiEventHandler s_connectedHandler;
WiFiEventHandler s_disconnectedHandler;
WiFiEventHandler s_gotIpHandler;
WiFiEventHandler s_dhcpHandler;
WiFiEventHandler s_modeHandler;
WiFiEventHandler s_authHandler;

const char *kindName(Kind kind) {
  switch (kind) {
    case Kind::Boot: return "boot";
    case Kind::Connected: return "connected";
    case Kind::Disconnected: return "disconnected";
    case Kind::GotIp: return "got_ip";
    case Kind::DhcpTimeout: return "dhcp_timeout";
    case Kind::Mode: return "mode";
    case Kind::AuthMode: return "authmode";
    case Kind::RetryBegin: return "retry_begin";
    case Kind::Retry: return "retry";
    case Kind::State: return "state";
    case Kind::Heartbeat: return "heartbeat";
    case Kind::Overflow: return "overflow";
  }
  return "unknown";
}

Event eventNow(Kind kind) {
  Event event{};
  event.kind = kind;
  event.ms = millis();
  return event;
}

void enqueue(const Event &event) {
  if (!s_enabled) return;
  esp8266::InterruptLock lock;
  if (s_count == QUEUE_SIZE) {
    ++s_dropped;
    return; // Keep earlier events, including the original outage reason.
  }
  s_queue[s_head] = event;
  s_head = (s_head + 1) % QUEUE_SIZE;
  ++s_count;
}

bool dequeue(Event &event) {
  esp8266::InterruptLock lock;
  if (!s_count) return false;
  event = s_queue[s_tail];
  s_tail = (s_tail + 1) % QUEUE_SIZE;
  --s_count;
  return true;
}

uint32_t droppedCount() {
  esp8266::InterruptLock lock;
  return s_dropped;
}

bool fail(const char *message) {
  s_enabled = false;
  snprintf(s_error, sizeof(s_error), "%s", message);
  Serial.printf("[wifi-diag] disabled: %s\n", s_error);
  return false;
}

Snapshot capture() {
  Snapshot state{};
  state.ms = millis();
  state.sdk = static_cast<uint8_t>(wifi_station_get_connect_status());
  state.status = static_cast<uint8_t>(WiFi.status());
  state.mode = static_cast<uint8_t>(WiFi.getMode());
  state.dhcp = static_cast<uint8_t>(wifi_station_dhcpc_status());
  state.channel = wifi_get_channel();
  state.autoReconnect = WiFi.getAutoReconnect();
  state.autoConnect = WiFi.getAutoConnect();
  state.rssi = WiFi.RSSI();
  state.heap = ESP.getFreeHeap();
  state.block = ESP.getMaxFreeBlockSize();
  ip_info ip{};
  if (wifi_get_ip_info(STATION_IF, &ip)) state.ip = ip.ip.addr;
  station_config config{};
  state.configBssidSet = -1;
  if (wifi_station_get_config(&config)) {
    state.configBssidSet = config.bssid_set ? 1 : 0;
    if (config.bssid_set) memcpy(state.configBssid, config.bssid, 6);
  }
  // config contains credentials; no field containing them enters the log.
  memset(&config, 0, sizeof(config));
  return state;
}

bool changed(const Snapshot &a, const Snapshot &b) {
  return a.sdk != b.sdk || a.status != b.status || a.mode != b.mode ||
         a.dhcp != b.dhcp || a.ip != b.ip ||
         a.autoReconnect != b.autoReconnect || a.autoConnect != b.autoConnect ||
         a.configBssidSet != b.configBssidSet ||
         memcmp(a.configBssid, b.configBssid, 6) != 0;
}

struct Line {
  size_t used = 0;
  bool valid = true;

  void add(const char *format, ...) {
    if (!valid) return;
    va_list args;
    va_start(args, format);
    int count = vsnprintf(s_line + used, sizeof(s_line) - used, format, args);
    va_end(args);
    if (count < 0 || static_cast<size_t>(count) >= sizeof(s_line) - used) {
      valid = false;
      return;
    }
    used += static_cast<size_t>(count);
  }

  void ip(const char *key, uint32_t address) {
    IPAddress value(address);
    add(",\"%s\":\"%u.%u.%u.%u\"", key,
        value[0], value[1], value[2], value[3]);
  }

  void bssid(const char *key, const uint8_t *value) {
    add(",\"%s\":\"%02x:%02x:%02x:%02x:%02x:%02x\"", key,
        value[0], value[1], value[2], value[3], value[4], value[5]);
  }
};

// An interrupted append may leave one partial record. Trim only that unfinished
// tail of our own file before adding the new boot, keeping complete records.
bool repairTail() {
  if (!LittleFS.exists(LOG_PATH)) return true;
  File file = LittleFS.open(LOG_PATH, "r+");
  if (!file) return fail("cannot open current log");
  size_t end = file.size();
  if (end > LOG_LIMIT) return fail("existing log exceeds limit");
  if (!end) return true;
  char tail[128];
  while (end) {
    size_t begin = end > sizeof(tail) ? end - sizeof(tail) : 0;
    size_t count = end - begin;
    if (!file.seek(begin) || file.readBytes(tail, count) != count)
      return fail("cannot read log tail");
    for (size_t i = count; i > 0; --i) {
      if (tail[i - 1] != '\n') continue;
      size_t complete = begin + i;
      if (complete != file.size() && !file.truncate(complete))
        return fail("cannot repair log tail");
      return true;
    }
    end = begin;
  }
  return file.truncate(0) || fail("cannot repair empty log");
}

bool append(const Line &line) {
  if (!line.valid) return fail("record exceeds buffer");
  File file = LittleFS.open(LOG_PATH, "a");
  if (!file) return fail("cannot append log");
  if (file.size() + line.used > LOG_LIMIT) {
    file.close();
    if (LittleFS.exists(PREVIOUS_PATH) && !LittleFS.remove(PREVIOUS_PATH))
      return fail("cannot remove previous log");
    if (!LittleFS.rename(LOG_PATH, PREVIOUS_PATH))
      return fail("cannot rotate log");
    file = LittleFS.open(LOG_PATH, "a");
    if (!file) return fail("cannot open rotated log");
  }
  FSInfo info;
  if (!LittleFS.info(info)) return fail("LittleFS is unavailable");
  // LittleFS allocates whole blocks, including copy-on-write metadata. Keep
  // headroom for a data block and metadata rather than counting just JSON bytes.
  size_t headroom = FS_RESERVE + line.used + 2 * info.blockSize;
  if (info.usedBytes > info.totalBytes ||
      info.totalBytes - info.usedBytes < headroom)
    return fail("insufficient diagnostic storage reserve");
  size_t previousSize = file.size();
  size_t written = file.write(reinterpret_cast<const uint8_t *>(s_line), line.used);
  file.flush();
  bool ok = written == line.used && !file.getWriteError();
  file.close();
  if (!ok) return fail("log write failed");
  // ESP8266 File::flush()/close() return no error; LittleFS only prints sync
  // failures to debug output. Reopen and verify this append without loading the
  // log into RAM. This detects observable commit failures, not sudden power loss.
  File verify = LittleFS.open(LOG_PATH, "r");
  if (!verify || verify.size() != previousSize + line.used || !verify.seek(previousSize))
    return fail("cannot verify committed log size");
  char check[64];
  for (size_t offset = 0; offset < line.used;) {
    size_t count = line.used - offset;
    if (count > sizeof(check)) count = sizeof(check);
    if (verify.readBytes(check, count) != count || memcmp(check, s_line + offset, count))
      return fail("cannot verify committed log data");
    offset += count;
  }
  return true;
}

bool writeRecord(const Event &event, const Snapshot &state) {
  if (event.kind == Kind::Disconnected) {
    s_lastReason = event.reason;
    if (!s_inOutage) {
      s_firstReason = event.reason;
      s_firstReasonMs = event.ms;
      s_inOutage = true;
    }
  } else if (event.kind == Kind::GotIp) {
    s_inOutage = false;
  }
  Line line;
  line.add("{\"boot\":\"%08lx\",\"seq\":%lu,\"t_ms\":%lu,\"sample_ms\":%lu,"
           "\"event\":\"%s\",\"sdk\":%u,\"status\":%u,\"mode\":%u,\"dhcp\":%u",
           static_cast<unsigned long>(s_boot), static_cast<unsigned long>(s_sequence++),
           static_cast<unsigned long>(event.ms), static_cast<unsigned long>(state.ms),
           kindName(event.kind), state.sdk, state.status, state.mode, state.dhcp);
  line.ip("ip", state.ip);
  line.add(",\"channel\":%u,\"rssi\":%ld,\"heap\":%lu,\"block\":%lu,"
           "\"config_bssid_set\":%d,\"dropped\":%lu,\"max_loop_gap_ms\":%lu,"
           "\"outage\":%s,\"first_reason\":%u,\"first_reason_ms\":%lu,\"last_reason\":%u,"
           "\"auto_reconnect\":%s,\"auto_connect\":%s",
           state.channel, static_cast<long>(state.rssi),
           static_cast<unsigned long>(state.heap), static_cast<unsigned long>(state.block),
           state.configBssidSet, static_cast<unsigned long>(droppedCount()),
           static_cast<unsigned long>(s_maxLoopGap), s_inOutage ? "true" : "false",
           s_firstReason, static_cast<unsigned long>(s_firstReasonMs), s_lastReason,
           state.autoReconnect ? "true" : "false", state.autoConnect ? "true" : "false");
  if (state.configBssidSet > 0) line.bssid("configured_bssid", state.configBssid);
  time_t utc = time(nullptr);
  if (utc >= TLS_TIME_VALID_FROM)
    line.add(",\"unix_s\":%lu", static_cast<unsigned long>(utc));
  switch (event.kind) {
    case Kind::Boot: {
      const rst_info *reset = ESP.getResetInfoPtr();
      line.add(",\"device\":\"%s\",\"version\":\"%s\",\"sdk_version\":\"%s\",\"reset\":%lu",
               deviceId(), FW_VERSION, ESP.getSdkVersion(),
               static_cast<unsigned long>(reset ? reset->reason : 255));
      break;
    }
    case Kind::Connected:
      line.add(",\"event_channel\":%u", event.channel);
      line.bssid("event_bssid", event.bssid);
      break;
    case Kind::Disconnected:
      line.add(",\"reason\":%u", event.reason);
      line.bssid("event_bssid", event.bssid);
      break;
    case Kind::GotIp:
      line.ip("event_ip", event.ip);
      line.ip("event_mask", event.mask);
      line.ip("event_gw", event.gateway);
      break;
    case Kind::Mode:
    case Kind::AuthMode:
      line.add(",\"old\":%u,\"new\":%u", event.oldMode, event.newMode);
      break;
    case Kind::Retry:
      line.add(",\"result\":%s", event.result ? "true" : "false");
      break;
    default: break;
  }
  line.add("}\n");
  // USB remains available when neither STA nor the fallback AP is reachable.
  // Only diagnostic builds include this output; records contain no credentials.
  Serial.write(reinterpret_cast<const uint8_t *>(s_line), line.used);
  return append(line);
}
} // namespace

void wifiDiagnosticsBegin() {
  if (s_started) return;
  s_started = true;
  FSInfo info;
  if (!LittleFS.info(info)) { fail("LittleFS is unavailable"); return; }
  if (!repairTail()) return;
  s_boot = ESP.random();
  s_enabled = true;
  s_connectedHandler = WiFi.onStationModeConnected([](const WiFiEventStationModeConnected &data) {
    Event event = eventNow(Kind::Connected);
    event.channel = data.channel;
    memcpy(event.bssid, data.bssid, 6);
    enqueue(event);
  });
  s_disconnectedHandler = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected &data) {
    Event event = eventNow(Kind::Disconnected);
    event.reason = static_cast<uint8_t>(data.reason);
    memcpy(event.bssid, data.bssid, 6);
    enqueue(event);
  });
  s_gotIpHandler = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP &data) {
    Event event = eventNow(Kind::GotIp);
    event.ip = static_cast<uint32_t>(data.ip);
    event.mask = static_cast<uint32_t>(data.mask);
    event.gateway = static_cast<uint32_t>(data.gw);
    enqueue(event);
  });
  s_dhcpHandler = WiFi.onStationModeDHCPTimeout([]() {
    enqueue(eventNow(Kind::DhcpTimeout));
  });
  s_modeHandler = WiFi.onWiFiModeChange([](const WiFiEventModeChange &data) {
    Event event = eventNow(Kind::Mode);
    event.oldMode = static_cast<uint8_t>(data.oldMode);
    event.newMode = static_cast<uint8_t>(data.newMode);
    enqueue(event);
  });
  s_authHandler = WiFi.onStationModeAuthModeChanged([](const WiFiEventStationModeAuthModeChanged &data) {
    Event event = eventNow(Kind::AuthMode);
    event.oldMode = data.oldMode;
    event.newMode = data.newMode;
    enqueue(event);
  });
  s_previous = capture();
  s_heartbeatMs = s_previous.ms;
  s_lastLoopMs = s_previous.ms;
  writeRecord(eventNow(Kind::Boot), s_previous);
}

void wifiDiagnosticsRetry(bool result) {
  Event event = eventNow(Kind::Retry);
  event.result = result;
  enqueue(event);
}

void wifiDiagnosticsBeforeRetry() {
  if (!s_enabled) return;
  // Preserve pending SDK events (especially the original disconnect) ahead of
  // this marker. The bounded drain also prevents an event storm blocking a retry.
  Snapshot state = capture();
  Event event;
  for (uint8_t i = 0; i < DRAIN_LIMIT && dequeue(event); ++i) {
    if (!writeRecord(event, state)) return;
  }
  state = capture();
  event = eventNow(Kind::RetryBegin);
  event.ms = state.ms;
  writeRecord(event, state);
}

void wifiDiagnosticsLoop() {
  if (!s_enabled) return;
  Snapshot state = capture();
  uint32_t gap = state.ms - s_lastLoopMs;
  s_lastLoopMs = state.ms;
  if (gap > s_maxLoopGap) s_maxLoopGap = gap;
  Event event;
  for (uint8_t i = 0; i < DRAIN_LIMIT && dequeue(event); ++i) {
    if (!writeRecord(event, state)) return;
  }
  if (changed(state, s_previous)) {
    event = eventNow(Kind::State);
    event.ms = state.ms;
    if (!writeRecord(event, state)) return;
  }
  s_previous = state;
  uint32_t dropped = droppedCount();
  if (dropped != s_reportedDropped) {
    event = eventNow(Kind::Overflow);
    event.ms = state.ms;
    if (!writeRecord(event, state)) return;
    s_reportedDropped = dropped;
  }
  if (uint32_t(state.ms - s_heartbeatMs) >= HEARTBEAT_MS) {
    s_heartbeatMs = state.ms;
    event = eventNow(Kind::Heartbeat);
    event.ms = state.ms;
    writeRecord(event, state);
  }
}

bool wifiDiagnosticsEnabled() { return s_enabled; }
const char *wifiDiagnosticsError() { return s_error; }
const char *wifiDiagnosticsLogPath() { return LOG_PATH; }
const char *wifiDiagnosticsPreviousLogPath() { return PREVIOUS_PATH; }

#endif // WPS_WIFI_DIAGNOSTICS
