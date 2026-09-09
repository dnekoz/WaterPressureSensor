#include "Mqtt.h"

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecureBearSSL.h>

#include <time.h>

#include <memory>

#include "CaStore.h"
#include "Config.h"
#include "Logic.h"
#include "Settings.h"

// Клиент создаётся под текущий режим и живёт до смены настроек. Держать
// одновременно и обычный WiFiClient, и BearSSL::WiFiClientSecure расточительно:
// на ESP8266 память — единственное, что реально ограничивает TLS.
static std::unique_ptr<WiFiClient> s_net;
static PubSubClient                client;
static WiFiClient                 disconnectedClient;
static BearSSL::WiFiClientSecure  *s_tlsClient = nullptr;
static bool s_discoveryPending = false;
static bool s_onlinePending = false;
static unsigned long s_lastDiscoveryMs = 0;

// Возобновление сессии: повторное подключение к тому же брокеру обходится без
// полного рукопожатия — это и секунды, и заметно меньший пик памяти.
static BearSSL::Session s_session;

// Разобранный CA живёт всё время работы (~1.5 КБ кучи): парсить его заново на
// каждую попытку значило бы читать и разбирать файл раз в 5 секунд.
static BearSSL::X509List *s_ca = nullptr;

static unsigned long s_lastAttemptMs = 0;
static unsigned long s_backoffMs     = MQTT_RECONNECT_MIN_MS;
static bool          s_everAttempted = false;
static bool          s_justConnected = false;
static bool          s_wasConnected = false;
static bool          s_waitingWifi = false;
static unsigned long s_attemptNumber = 0;

// Предварительный DNS даёт понятную ошибку и адрес для пробы MFLN.
// TLS всё равно получает имя хоста, необходимое для SNI и проверки сертификата.
static IPAddress s_brokerIp;
static bool      s_brokerResolved = false;

// Результат пробы MFLN: поддерживает ли брокер укороченные записи TLS. От этого
// зависит, нужен ли непрерывный блок в 16 КБ или хватит одного.
static bool s_mflnProbed = false;
static bool s_mflnOk     = false;

static char s_tlsError[192]  = "";
static char s_mqttError[192] = "";

// Брокер отверг сами настройки. Повторять до их правки бессмысленно, а каждая
// попытка на секунды останавливает loop() вместе с веб-порталом — то есть мешает
// ровно тому, чем эти настройки и исправляют.
static bool s_configRejected = false;

bool mqttEnabled() { return settings.mqttHost[0] != '\0'; }

bool mqttConnected() { return client.connected(); }

bool mqttTlsActive() { return s_tlsClient && client.connected(); }

const char *mqttTlsError() { return s_tlsError; }

const char *mqttLastError() { return s_mqttError; }

static void setMqttError(const char *msg) {
  strncpy(s_mqttError, msg, sizeof(s_mqttError) - 1);
  s_mqttError[sizeof(s_mqttError) - 1] = '\0';
}

static void setTlsError(const char *msg) {
  strncpy(s_tlsError, msg, sizeof(s_tlsError) - 1);
  s_tlsError[sizeof(s_tlsError) - 1] = '\0';
  if (msg[0]) {
    Serial.printf("[tls] %s\n", s_tlsError);
    setMqttError(s_tlsError);
  }
}

static void reportFailure(int state) {
  const MqttDiagnostic diagnostic = mqttDiagnostic(state);
  strncpy_P(s_mqttError, diagnostic.reason, sizeof(s_mqttError) - 1);
  s_mqttError[sizeof(s_mqttError) - 1] = '\0';
  Serial.printf("[MQTT][ERROR] %s:%u, код %d: %s\n", settings.mqttHost, settings.mqttPort, state, s_mqttError);
  Serial.print(F("[MQTT][CHECK] "));
  Serial.println(reinterpret_cast<const __FlashStringHelper *>(diagnostic.action));
  if (diagnostic.permanent) {
    s_configRejected = true;
    Serial.println(F("[MQTT][PAUSED] Повторы остановлены. Исправьте настройки в веб-интерфейсе и нажмите Сохранить MQTT."));
  }
}

// Часы нужны только режиму TLS_CA: проверка срока действия сертификата без них
// провалится всегда, и ошибка BearSSL на настоящую причину не укажет.
static bool timeIsSet() { return (unsigned long)time(nullptr) >= TLS_TIME_VALID_FROM; }

void mqttBegin() {
  // У PubSubClient 2.8 буфер по умолчанию 256 байт. Discovery-payload Home
  // Assistant в него не помещается, и publish() молча возвращает false —
  // именно так эта связка обычно и «не работает без ошибок».
  if (!client.setBufferSize(MQTT_BUFFER_SIZE)) {
    Serial.println(F("[mqtt] не хватило памяти на буфер, discovery может не пройти"));
  }
  client.setKeepAlive(30);
  client.setSocketTimeout(MQTT_SOCKET_TIMEOUT_S);

  mqttApplySettings();
}

// Создаёт клиент под текущий режим. Старый уничтожается здесь же: без этого
// каждое сохранение настроек с портала оставляло бы за собой утечку.
static void rebuildClient() {
  // Именно connected(), а не безусловный disconnect(): PubSubClient::disconnect()
  // разыменовывает свой Client без проверки на NULL, а при первом вызове (из
  // mqttBegin()) клиент ещё не назначен. connected() к NULL устойчив.
  if (client.connected()) client.disconnect();
  client.setClient(disconnectedClient);
  s_tlsClient = nullptr;
  s_net.reset();

  delete s_ca;
  s_ca = nullptr;

  // Сессия привязана к конкретному брокеру: после смены настроек предлагать её
  // заново бессмысленно, а на чужом сервере она только удлинит рукопожатие.
  s_session = BearSSL::Session();

  s_tlsError[0] = '\0';

  if (settings.mqttTlsMode == TLS_OFF) {
    s_net.reset(new WiFiClient());
    s_net->setTimeout(MQTT_TCP_TIMEOUT_MS);
    client.setClient(*s_net);
    return;
  }

  BearSSL::WiFiClientSecure *tls = new BearSSL::WiFiClientSecure();
  s_tlsClient = tls;
  tls->setTimeout(MQTT_TCP_TIMEOUT_MS);
  tls->setSession(&s_session);

  switch (settings.mqttTlsMode) {
    case TLS_INSECURE:
      // Шифрование без проверки: защищает от прослушивания, но не от подмены
      // брокера. Осознанный выбор пользователя, а не значение по умолчанию.
      tls->setInsecure();
      Serial.println(F("[tls] режим: без проверки сертификата"));
      break;

    case TLS_FINGERPRINT: {
      uint8_t fp[TLS_FINGERPRINT_LEN];
      if (parseFingerprint(settings.mqttFingerprint, fp)) {
        tls->setFingerprint(fp);
        Serial.printf("[tls] режим: пиннинг по отпечатку %s\n", settings.mqttFingerprint);
      } else {
        setTlsError("Неверный отпечаток. TLS-подключение остановлено.");
        s_configRejected = true;
      }
      break;
    }

    case TLS_CA:
      s_ca = caLoad();
      if (s_ca) {
        tls->setTrustAnchors(s_ca);
        Serial.println(F("[tls] режим: проверка по загруженному CA"));
      } else {
        // Без CA подключаться нельзя: пользователь просил именно проверку.
        // Тихо откатиться в insecure значило бы обмануть его ожидания.
        setTlsError(caLastError());
        Serial.println(F("[TLS][CA] Для выбранного режима нужен читаемый CA-сертификат. Откройте Подключение → CA-сертификат и загрузите PEM."));
      }
      break;

    default:
      break;
  }

  s_net.reset(tls);
  client.setClient(*s_net);

  // Проба сама открывает TCP-соединение, поэтому только при смене настроек.
  s_mflnProbed = false;
}

void mqttApplySettings() {
  s_backoffMs      = MQTT_RECONNECT_MIN_MS;
  s_lastAttemptMs  = 0;
  s_everAttempted  = false;
  s_brokerResolved = false;
  s_justConnected = false;
  s_discoveryPending = false;
  s_onlinePending = false;
  s_wasConnected = false;
  s_waitingWifi = false;
  s_attemptNumber = 0;

  // Настройки изменили — значит, есть смысл попробовать снова.
  s_configRejected = false;
  s_mqttError[0]   = '\0';

  rebuildClient();

  if (const char *error = settingsValidationError(settings)) {
    setMqttError(error);
    s_configRejected = true;
    Serial.printf("[MQTT][CONFIG] Настройки не применены: %s\n", error);
    Serial.println(F("[MQTT][PAUSED] Откройте Подключение в веб-интерфейсе, исправьте поля MQTT и сохраните."));
    return;
  }

  if (!mqttEnabled()) {
    Serial.println(F("[MQTT][OFF] Адрес брокера пуст. MQTT выключен; измерения и веб-интерфейс работают автономно."));
    return;
  }

  Serial.printf("[mqtt] брокер %s:%u%s, топик %s\n", settings.mqttHost, settings.mqttPort,
                settings.mqttTlsMode == TLS_OFF ? "" : " (TLS)", settings.mqttBase);
}

// Одна сущность Home Assistant.
static bool publishDiscovery(const char *key, const char *name, const char *devClass,
                             const char *unit, const char *valueTemplate, bool diagnostic) {
  char cfgTopic[160];
  mqttDiscoveryTopic(cfgTopic, sizeof(cfgTopic), settings, deviceId(), key);

  char configUrl[48];
  snprintf(configUrl, sizeof(configUrl), "http://%s/", WiFi.localIP().toString().c_str());

  // Статический, а не стековый: у ESP8266 стек рабочего контекста всего ~4 КБ,
  // а функция не реентерабельна.
  static char payload[MQTT_BUFFER_SIZE];
  size_t len = mqttDiscoveryJson(payload, sizeof(payload), settings, deviceId(), key, name,
                                 devClass, unit, valueTemplate, diagnostic, configUrl);

  if (!len || !client.publish(cfgTopic, payload, true)) {
    Serial.printf("[MQTT][DISCOVERY] Не удалось отправить %s (%u байт). Проверьте соединение и размер MQTT-буфера; будет повтор.\n", key,
                  (unsigned)len);
    return false;
  }
  return true;
}

static bool discoveryWriter(const char *prefix, const char *key, bool remove, void *) {
  if (remove) {
    char topic[160];
    mqttDiscoveryTopicFor(topic, sizeof(topic), prefix, deviceId(), key);
    return client.publish(topic, "", true);
  }
  if (!strcmp(key, "pressure")) return publishDiscovery(key, "Pressure", "pressure", "bar", "{{ value_json.bar }}", false);
  if (!strcmp(key, "voltage")) return publishDiscovery(key, "Sensor voltage", "voltage", "V", "{{ value_json.volt }}", false);
  if (!strcmp(key, "rssi")) return publishDiscovery(key, "WiFi signal", "signal_strength", "dBm", "{{ value_json.rssi }}", true);
  return publishDiscovery(key, "ADC raw", nullptr, nullptr, "{{ value_json.raw }}", true);
}

static bool discoveryPersist(const Settings &candidate, void *) { return settingsSave(candidate); }

static void syncDiscovery() {
  if (s_onlinePending) {
    char topic[96];
    mqttTopic(topic, sizeof(topic), settings, "status");
    s_onlinePending = !client.publish(topic, "online", true);
  }
  if (s_discoveryPending)
    s_discoveryPending = !mqttReconcileDiscovery(settings, discoveryWriter, discoveryPersist, nullptr);
  s_lastDiscoveryMs = millis();
}
// Литеральный адрес hostByName() разбирает сам, отдельная ветка под IP не нужна.
static bool resolveBroker() {
  if (s_brokerResolved) return true;

  if (!WiFi.hostByName(settings.mqttHost, s_brokerIp, MQTT_DNS_TIMEOUT_MS)) {
    Serial.printf("[MQTT][DNS] Не найден адрес брокера '%s'. Проверьте написание имени, DNS роутера или укажите IP вместо имени.\n", settings.mqttHost);
    setMqttError("Не удалось определить IP брокера. Проверьте адрес и DNS.");
    return false;
  }

  Serial.printf("[mqtt] %s -> %s\n", settings.mqttHost, s_brokerIp.toString().c_str());
  // BearSSL connect(IPAddress) не передаёт имя для SAN/CN и SNI.
  // TLS должен подключаться именно через перегрузку с именем хоста.
  if (s_tlsClient) client.setServer(settings.mqttHost, settings.mqttPort);
  else client.setServer(s_brokerIp, settings.mqttPort);
  s_brokerResolved = true;
  return true;
}

// Готовит TLS-клиент к рукопожатию. false, если начинать бессмысленно.
static bool prepareTls() {
  BearSSL::WiFiClientSecure *tls = s_tlsClient;
  if (!tls || s_configRejected) return false;

  if (settings.mqttTlsMode == TLS_CA) {
    if (!s_ca) {
      // Сертификат мог появиться после старта — его только что загрузили с портала.
      s_ca = caLoad();
      if (!s_ca) {
        setTlsError(caLastError());
        Serial.println(F("[TLS][CA] Загрузите корневой CA в разделе Подключение. Без него проверка сертификата брокера невозможна."));
        return false;
      }
      tls->setTrustAnchors(s_ca);
    }

    if (!timeIsSet()) {
      setTlsError("Время ещё не установлено. TLS по CA ожидает синхронизации NTP.");
      Serial.printf("[TLS][TIME] NTP: %s. Проверьте доступ к серверу времени (UDP 123) или укажите локальный NTP.\n",
                    settings.ntpServer[0] ? settings.ntpServer : NTP_SERVER_DEFAULT);
      return false;
    }
    tls->setX509Time(time(nullptr));
  }

  if (!s_mflnProbed) {
    // Укороченные записи TLS сокращают приёмный буфер с 16 КБ до одного, то есть
    // делают разницу между «работает» и «не хватает памяти». Поддерживает их
    // далеко не всякий брокер, поэтому спрашиваем, а не предполагаем.
    s_mflnOk = BearSSL::WiFiClientSecure::probeMaxFragmentLength(s_brokerIp, settings.mqttPort,
                                                                 TLS_RX_MFLN);
    s_mflnProbed = true;
    Serial.printf("[tls] MFLN %s, приёмный буфер %d байт\n", s_mflnOk ? "поддерживается" : "нет",
                  s_mflnOk ? TLS_RX_MFLN : TLS_RX_FULL);
  }

  tls->setBufferSizes(s_mflnOk ? TLS_RX_MFLN : TLS_RX_FULL, TLS_TX_SIZE);

  const size_t need  = s_mflnOk ? TLS_MIN_BLOCK_MFLN : TLS_MIN_BLOCK_FULL;
  const size_t block = ESP.getMaxFreeBlockSize();
  if (block < need) {
    char msg[160];
    snprintf(msg, sizeof(msg), "мало непрерывной памяти: %u из %u", (unsigned)block,
             (unsigned)need);
    setTlsError(msg);
    return false;
  }

  return true;
}

// false, если попытка не удалась: вызывающий увеличит backoff.
static bool tryConnect() {
  ++s_attemptNumber;
  Serial.printf("[MQTT][CONNECT] Попытка %lu: %s:%u, %s, Client ID=%s.\n", s_attemptNumber,
                settings.mqttHost, settings.mqttPort, s_tlsClient ? "TLS" : "обычный TCP", deviceId());
  if (!resolveBroker()) return false;
  if (s_tlsClient && !prepareTls()) return false;

  char availTopic[96];
  mqttTopic(availTopic, sizeof(availTopic), settings, "status");

  // Пустые логин/пароль надо передавать как NULL: пустая строка означала бы
  // попытку аутентификации с пустым пользователем, и брокер её отклонит.
  const char *user = settings.mqttUser[0] ? settings.mqttUser : nullptr;
  const char *pass = settings.mqttPass[0] ? settings.mqttPass : nullptr;

  const unsigned long startedMs = millis();

  // Последняя воля: если устройство пропадёт, брокер сам разошлёт "offline".
  if (!client.connect(deviceId(), user, pass, availTopic, 0, true, "offline")) {
    const int state = client.state();
    reportFailure(state);

    if (s_tlsClient) {
      // Без этого «не подключается» неотличимо от неверного пароля.
      char buf[128];
      BearSSL::WiFiClientSecure *tls = s_tlsClient;
      int err = tls->getLastSSLError(buf, sizeof(buf));
      if (err) {
        setTlsError("Не удалось установить TLS. Подробности и подсказка выведены в Serial.");
        Serial.printf("[TLS][ERROR] Код %d: %s\n", err, buf);
        if (settings.mqttTlsMode == TLS_CA)
          Serial.println(F("[TLS][CHECK] Проверьте корневой CA, срок сертификата, часы NTP и совпадение адреса брокера с именем в сертификате."));
        else if (settings.mqttTlsMode == TLS_FINGERPRINT)
          Serial.println(F("[TLS][CHECK] Сверьте SHA-1 с текущим сертификатом брокера. После перевыпуска сертификата отпечаток меняется."));
        else
          Serial.println(F("[TLS][CHECK] Убедитесь, что на выбранном порту включён TLS и брокер поддерживает совместимые параметры TLS."));
      }
    }

    // Адрес мог смениться (DHCP на стороне брокера) — разрешаем имя заново.
    s_brokerResolved = false;
    return false;
  }

  if (settings.mqttTlsMode != TLS_OFF) {
    s_tlsError[0] = '\0';
    Serial.printf("[tls] рукопожатие заняло %lu мс, свободный блок %u\n", millis() - startedMs,
                  ESP.getMaxFreeBlockSize());
  }
  s_mqttError[0] = '\0';
  Serial.printf("[MQTT][OK] Подключено к %s:%u. Показания: %s/state, интервал %u с.\n",
                settings.mqttHost, settings.mqttPort, settings.mqttBase, settings.mqttIntervalS);
  s_wasConnected = true;

  s_onlinePending = true;
  s_discoveryPending = true;
  syncDiscovery();
  s_justConnected = true;
  return true;
}

void mqttLoop() {
  if (!mqttEnabled()) return;
  if (WiFi.status() != WL_CONNECTED) {
    if (!s_waitingWifi) {
      Serial.println(F("[MQTT][WIFI] Нет подключения к Wi-Fi. Ожидаю сеть; настройки брокера пока не проверяются."));
      setMqttError("Нет Wi-Fi. MQTT подключится после восстановления сети.");
      s_waitingWifi = true;
    }
    return;
  }
  if (s_waitingWifi) {
    Serial.println(F("[MQTT][WIFI] Wi-Fi восстановлен. Продолжаю подключение к брокеру."));
    s_waitingWifi = false;
  }

  if (s_wasConnected && !client.connected()) {
    s_wasConnected = false;
    s_brokerResolved = false;
    reportFailure(MQTT_CONNECTION_LOST);
  }

  if (client.connected()) {
    client.loop();
    if (client.connected() && (s_discoveryPending || s_onlinePending) &&
        millis() - s_lastDiscoveryMs >= 10000UL) syncDiscovery();
    return;
  }

  // Настройки отвергнуты брокером — молча ждём, пока их исправят на портале.
  // Снимается в mqttApplySettings().
  if (s_configRejected) return;

  // Первая попытка — сразу, дальше по backoff. Вычитание unsigned long
  // корректно переживает переполнение millis().
  if (s_everAttempted && (millis() - s_lastAttemptMs) < s_backoffMs) return;

  s_everAttempted = true;

  if (tryConnect()) {
    s_backoffMs = MQTT_RECONNECT_MIN_MS;
  } else {
    s_backoffMs = min(s_backoffMs * 2, (unsigned long)MQTT_RECONNECT_MAX_MS);
    if (!s_configRejected) Serial.printf("[MQTT][RETRY] Следующая попытка через %lu с. Веб-интерфейс доступен для исправления настроек.\n", s_backoffMs / 1000);
  }

  // Отсчёт backoff ведём от конца попытки, а не от начала: рукопожатие TLS
  // занимает секунды, и иначе пауза между попытками была бы меньше заданной.
  s_lastAttemptMs = millis();
}

void mqttPublishState(const Reading &r) {
  if (!client.connected()) return;

  char stateTopic[96];
  mqttTopic(stateTopic, sizeof(stateTopic), settings, "state");

  char payload[256];
  if (!mqttStateJson(payload, sizeof(payload), r, WiFi.RSSI(), ESP.getFreeHeap())) return;

  if (!client.publish(stateTopic, payload, false)) {
    Serial.println(F("[mqtt] состояние отправить не удалось"));
  }
}

bool mqttConsumeJustConnected() {
  bool just       = s_justConnected;
  s_justConnected = false;
  return just;
}
