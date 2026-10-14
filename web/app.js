'use strict';
const $ = id => document.getElementById(id);
const text = (id, value) => { $(id).textContent = value; };
const fmt = (value, digits = 2) => Number.isFinite(value) ? value.toLocaleString('ru-RU', {minimumFractionDigits: digits, maximumFractionDigits: digits}) : '—';
let statusData = null, settingsData = null, authenticated = false, settingsLoading = false;
let statusAt = 0, polling = false, pollTimer, uploading = false, reconnectUntil = 0;
const messages = (form, message, error = false) => {
  const target = form.querySelector('.form-message');
  target.textContent = message;
  target.classList.toggle('error', error);
};

const passwordFields = Array.from(document.querySelectorAll('input[type="password"]'));
function passwordVisibility(input, visible) {
  input.type = visible ? 'text' : 'password';
  const button = input.parentElement.querySelector('.password-toggle');
  const label = visible ? 'Скрыть пароль' : 'Показать пароль';
  button.setAttribute('aria-label', label);
  button.setAttribute('aria-pressed', String(visible));
  button.title = label;
  button.querySelector('use').setAttribute('href', visible ? '#i-eye-off' : '#i-eye');
}
passwordFields.forEach(input => {
  input.dataset.passwordField = '';
  const wrapper = document.createElement('div'); wrapper.className = 'password-field';
  input.before(wrapper); wrapper.append(input);
  const button = document.createElement('button');
  button.type = 'button'; button.className = 'password-toggle';
  button.setAttribute('aria-controls', input.id);
  const icon = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  icon.setAttribute('aria-hidden', 'true');
  icon.append(document.createElementNS('http://www.w3.org/2000/svg', 'use'));
  button.append(icon); wrapper.append(button);
  button.addEventListener('click', () => passwordVisibility(input, input.type === 'password'));
  passwordVisibility(input, false);
});
$('login-dialog').addEventListener('close', () => {
  passwordVisibility($('login-password'), false);
  notifyLoginDisplay(false);
});

function syncTheme() {
  const dark = document.documentElement.dataset.theme === 'dark';
  const label = `Включить ${dark ? 'светлую' : 'тёмную'} тему`;
  $('theme-toggle').setAttribute('aria-label', label);
  $('theme-toggle').title = label;
  $('theme-toggle').querySelector('use').setAttribute('href', dark ? '#i-sun' : '#i-moon');
}
$('theme-toggle').addEventListener('click', () => {
  const theme = document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark';
  document.documentElement.dataset.theme = theme;
  try { localStorage.setItem('wps-theme', theme); } catch (_) { /* Private storage may be unavailable. */ }
  syncTheme();
});
matchMedia('(prefers-color-scheme: dark)').addEventListener('change', event => {
  try { if (localStorage.getItem('wps-theme')) return; } catch (_) { /* Follow OS without storage. */ }
  document.documentElement.dataset.theme = event.matches ? 'dark' : 'light';
  syncTheme();
});
syncTheme();

function navigate() {
  const requested = location.hash.slice(1);
  const section = ['overview', 'connection', 'calibration', 'device'].includes(requested) ? requested : 'overview';
  document.querySelectorAll('.page').forEach(page => { page.hidden = page.id !== section; });
  document.querySelectorAll('.navigation a').forEach(link => {
    if (link.hash === '#' + section) link.setAttribute('aria-current', 'page');
    else link.removeAttribute('aria-current');
  });
  document.title = `${$(section + '-title').textContent} · Давление воды`;
  if (authenticated && !settingsData) loadSettings();
}
window.addEventListener('hashchange', () => { navigate(); window.scrollTo(0, 0); });
navigate();

async function api(path, {method = 'GET', body, timeout = 8000, loginOn401 = true} = {}) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeout);
  const headers = {};
  if (method !== 'GET') headers['X-WPS-Request'] = '1';
  if (body && !(body instanceof FormData)) { headers['Content-Type'] = 'application/json'; body = JSON.stringify(body); }
  try {
    const response = await fetch(path, {method, body, headers, signal: controller.signal, cache: 'no-store', credentials: 'same-origin'});
    let data;
    try { data = await response.json(); } catch (_) { throw new Error('Устройство вернуло неожиданный ответ. Обновите страницу.'); }
    if (!response.ok) {
      if (response.status === 401 && loginOn401) { setAuth(false); showLogin(); }
      throw new Error(data.error || `Ошибка устройства (${response.status}).`);
    }
    return data;
  } catch (error) {
    if (error.name === 'AbortError') throw new Error('Устройство не ответило вовремя. Проверьте соединение и повторите действие.');
    if (error instanceof TypeError) throw new Error('Нет связи с устройством. Проверьте сеть Wi-Fi.');
    throw error;
  } finally { clearTimeout(timer); }
}

function setAuth(value) {
  const changed = authenticated !== value;
  authenticated = value;
  document.querySelectorAll('[data-protected]').forEach(fieldset => { fieldset.disabled = !value || settingsLoading; });
  document.querySelectorAll('.auth-notice').forEach(notice => { notice.hidden = value; });
  $('auth-button').classList.toggle('active', value);
  const label = value ? 'Выйти из настроек' : 'Войти в настройки';
  $('auth-button').title = label;
  $('auth-button').setAttribute('aria-label', label);
  if (changed && value && !settingsData) loadSettings();
}
let loginDisplayRequest = Promise.resolve();
function notifyLoginDisplay(visible) {
  // Последовательные запросы: быстрый крестик не должен обгонять показ пароля.
  loginDisplayRequest = loginDisplayRequest.then(() => api('/api/login/display', {
    method: 'POST', body: {visible}, loginOn401: false
  })).catch(error => {
    if ($('login-dialog').open) messages($('login-form'), error.message, true);
  });
  return loginDisplayRequest;
}
function showLogin() {
  if (!$('login-dialog').open) {
    messages($('login-form'), '');
    $('login-dialog').showModal();
    notifyLoginDisplay(true);
  }
}
document.querySelectorAll('[data-login]').forEach(button => button.addEventListener('click', showLogin));
document.querySelectorAll('[data-close]').forEach(button => button.addEventListener('click', () => button.closest('dialog').close()));
$('auth-button').addEventListener('click', async () => {
  if (!authenticated) return showLogin();
  try {
    await api('/api/logout', {method: 'POST'});
    passwordFields.forEach(input => { input.value = ''; passwordVisibility(input, false); });
    settingsData = null;
    setAuth(false);
  } catch (error) { text('connection-notice', error.message); $('connection-notice').hidden = false; }
});
$('login-form').addEventListener('submit', async event => {
  event.preventDefault();
  const form = event.currentTarget, button = form.querySelector('[type=submit]');
  button.disabled = true;
  messages(form, 'Проверяем пароль…');
  try {
    await loginDisplayRequest;
    await api('/api/login', {method: 'POST', body: {password: $('login-password').value}, loginOn401: false});
    $('login-password').value = '';
    $('login-dialog').close();
    setAuth(true);
  } catch (error) { messages(form, error.message, true); }
  finally { button.disabled = false; }
});

function confirmAction(title, description, buttonLabel) {
  text('confirm-title', title); text('confirm-text', description); text('confirm-accept', buttonLabel);
  return new Promise(resolve => {
    const dialog = $('confirm-dialog');
    dialog.returnValue = '';
    $('confirm-accept').onclick = () => dialog.close('yes');
    $('confirm-cancel').onclick = () => dialog.close();
    dialog.addEventListener('close', () => resolve(dialog.returnValue === 'yes'), {once: true});
    dialog.showModal();
  });
}

for (let i = 0; i <= 40; i++) {
  const angle = Math.PI - i * Math.PI / 40;
  const line = document.createElementNS('http://www.w3.org/2000/svg', 'line');
  const inner = i % 5 === 0 ? 217 : 225;
  line.setAttribute('x1', 300 + inner * Math.cos(angle)); line.setAttribute('y1', 300 - inner * Math.sin(angle));
  line.setAttribute('x2', 300 + 231 * Math.cos(angle)); line.setAttribute('y2', 300 - 231 * Math.sin(angle));
  $('gauge-ticks').appendChild(line);
}

function readingState() {
  if (!statusData || Date.now() - statusAt > 5000) return {valid: false, label: 'Нет свежих данных', detail: 'Проверьте связь с устройством'};
  const r = statusData.reading;
  if (r.stale || r.age_ms + (Date.now() - statusAt) > 5000) return {valid: false, label: 'Измерение устарело', detail: 'Ожидаем новый отсчёт АЦП'};
  if (r.over) return {valid: false, label: 'Выход за диапазон', detail: 'Давление неизвестно · проверьте входной сигнал', bad: true};
  if (r.fault) return {valid: false, label: 'Ошибка сигнала датчика', detail: 'Проверьте питание и подключение', bad: true};
  if (!Number.isFinite(r.bar)) return {valid: false, label: 'Измерение недоступно', detail: 'Ожидаем корректные данные'};
  return {valid: true, label: 'Сигнал в измеримом диапазоне', detail: 'Данные поступают'};
}
function renderReading() {
  const state = readingState(), r = statusData?.reading;
  const value = state.valid ? r.bar : null;
  const fraction = state.valid ? Math.min(1, Math.max(0, value / statusData.max_bar)) : 0;
  text('pressure-value', fmt(value)); text('pressure-mpa', `${fmt(state.valid ? r.mpa : null, 3)} МПа`);
  $('gauge').setAttribute('aria-label', state.valid ? `Давление ${fmt(value)} бар` : state.label);
  $('gauge-arc').setAttribute('stroke-dasharray', `${fraction * 100} 100`);
  $('gauge-arc').classList.toggle('empty', !state.valid);
  $('gauge-dot').toggleAttribute('hidden', !state.valid);
  $('pressure-value').classList.toggle('compact', state.valid && Math.abs(value) >= 100);
  const angle = Math.PI - fraction * Math.PI;
  $('gauge-dot').setAttribute('cx', 300 + 250 * Math.cos(angle));
  $('gauge-dot').setAttribute('cy', 300 - 250 * Math.sin(angle));
  $('measurement-state').className = `measurement-state ${state.valid ? 'good' : state.bad ? 'bad' : ''}`;
  $('measurement-state').lastElementChild.textContent = state.label;
  text('sensor-state', state.valid ? 'Сигнал получен' : state.label);
  text('sensor-detail', state.detail);
  $('sensor-dot').className = `status-dot ${state.valid ? 'good' : state.bad ? 'bad' : ''}`;
  const fresh = statusAt && Date.now() - statusAt <= 5000;
  $('live-badge').className = `badge ${fresh ? 'good' : ''}`;
  text('live-badge', fresh ? 'На связи' : statusAt ? 'Нет связи' : 'Подключаемся');
  text('reading-time', fresh ? `Обновлено ${new Date(statusAt).toLocaleTimeString('ru-RU')}` : 'Ожидание данных');
  if (!fresh) {
    ['wifi', 'mqtt'].forEach(key => { text(key + '-state', 'Нет свежих данных'); text(key + '-detail', 'Связь с интерфейсом потеряна'); $(key + '-dot').className = 'status-dot'; });
  }
  text('diag-volts', fresh && r ? `${fmt(r.volt, 3)} В` : '—');
  text('diag-raw', fresh && r ? `${r.raw} / ${r.raw_instant}` : '—');
  text('diag-rssi', fresh && statusData?.wifi.connected ? `${r.rssi} dBm` : '—');
  text('diag-heap', fresh && r ? `${fmt(r.heap / 1024, 1)} КБ` : '—');
}
function uptime(seconds) {
  const days = Math.floor(seconds / 86400), hours = Math.floor(seconds % 86400 / 3600), minutes = Math.floor(seconds % 3600 / 60);
  return `${days ? days + ' д ' : ''}${hours} ч ${minutes} мин`;
}
function renderStatus(data) {
  statusData = data; statusAt = Date.now();
  setAuth(data.authenticated);
  $('demo-notice').hidden = !data.demo;
  text('device-name', data.name); text('gauge-max', fmt(data.max_bar));
  text('gauge-range', `Измеримый диапазон: 0–${fmt(data.max_bar)} бар`);
  text('wifi-state', data.wifi.connected ? 'Подключено' : data.wifi.ap ? 'Резервная точка доступа' : 'Подключение к сети');
  text('wifi-detail', data.wifi.connected ? `${data.wifi.ssid} · ${data.wifi.ip}` : data.wifi.ap_name);
  $('wifi-dot').className = `status-dot ${data.wifi.connected ? 'good' : ''}`;
  text('mqtt-state', !data.mqtt.enabled ? 'Выключен' : data.mqtt.connected ? 'Подключено' : 'Нет соединения');
  text('mqtt-detail', !data.mqtt.enabled ? 'Автономный режим' : data.mqtt.connected ? data.mqtt.tls ? 'Соединение TLS' : 'Соединение без TLS' : data.tls_error || data.mqtt.error || 'Ожидаем подключение к брокеру');
  $('mqtt-dot').className = `status-dot ${data.mqtt.connected ? 'good' : data.mqtt.enabled ? 'bad' : ''}`;
  text('ca-state', data.ca.present ? `Сертификат загружен · ${fmt(data.ca.size / 1024, 1)} КБ` : 'Сертификат не загружен');
  if (data.ca.error) text('ca-state', `${$('ca-state').textContent}. ${data.ca.error}`);
  text('ap-name', data.wifi.ap_name);
  text('info-id', data.id); text('info-version', data.version);
  text('info-ip', data.wifi.connected ? data.wifi.ip : data.wifi.ap_ip);
  text('info-uptime', uptime(data.uptime_s)); text('info-block', `${fmt(data.heap_block / 1024, 1)} КБ`);
  text('footer-version', `Прошивка ${data.version}`);
  text('footer-address', data.wifi.connected ? data.wifi.ip : data.wifi.ap_ip);
  $('storage-notice').hidden = !data.storage_error;
  text('storage-notice', data.storage_error ? `Не удалось сохранить настройки: ${data.storage_error}` : '');
  $('connection-notice').hidden = true;
  renderReading();
}
async function poll() {
  clearTimeout(pollTimer);
  if (polling || uploading || document.hidden) return;
  polling = true;
  try { renderStatus(await api('/api/status', {timeout: 5000})); }
  catch (_) {
    renderReading();
    text('connection-notice', Date.now() < reconnectUntil ? 'Устройство перезапускается или меняет сеть. Ожидаем восстановления связи…' : 'Нет ответа от устройства. Проверьте Wi-Fi; соединение восстановится автоматически.');
    $('connection-notice').hidden = false;
  } finally { polling = false; pollTimer = setTimeout(poll, 2000); }
}
setInterval(() => { if (!document.hidden) renderReading(); }, 1000);
document.addEventListener('visibilitychange', () => {
  if (document.hidden) clearTimeout(pollTimer);
  else { renderReading(); poll(); }
});

async function loadSettings() {
  if (settingsLoading) return;
  settingsLoading = true; setAuth(authenticated);
  try {
    const data = await api('/api/settings');
    settingsData = data;
    for (const [key, value] of Object.entries(data)) {
      const input = $(key);
      if (input && /^(INPUT|SELECT)$/.test(input.tagName)) input.value = value;
    }
    $('wifi-ssid').value = data.wifiSsid;
    $('wifi-keep').checked = Boolean(data.wifiSsid);
    $('mqttPass').placeholder = data.mqttPasswordSet ? 'Сохранён · не изменять' : 'Не задан';
    updateWifiKeep(); updateTls(); updateCalibration();
  } catch (error) {
    text('connection-notice', error.message); $('connection-notice').hidden = false;
    // Retry through explicit navigation or login; keep fields locked until loaded.
  } finally { settingsLoading = false; setAuth(authenticated && Boolean(settingsData)); }
}
function updateWifiKeep() {
  const input = $('wifi-password');
  input.disabled = $('wifi-keep').checked;
  input.parentElement.querySelector('.password-toggle').disabled = input.disabled;
  if (input.disabled) passwordVisibility(input, false);
}
$('wifi-keep').addEventListener('change', updateWifiKeep);
$('wifi-ssid').addEventListener('input', () => { if ($('wifi-ssid').value !== settingsData?.wifiSsid) $('wifi-keep').checked = false; updateWifiKeep(); });
function updateTls() {
  const mode = Number($('mqttTlsMode').value);
  $('fingerprint-fields').hidden = mode !== 2;
  $('ntp-fields').hidden = mode !== 3;
  text('tls-help', [
    'Данные и пароль MQTT передаются без шифрования.',
    'Соединение шифруется, но подлинность брокера не проверяется.',
    'Отпечаток должен совпадать с сертификатом брокера. При его замене обновите отпечаток.',
    'Нужны корневой CA-сертификат, точное время NTP и адрес, совпадающий с именем в сертификате.'
  ][mode]);
}
$('mqttTlsMode').addEventListener('change', () => {
  const port = Number($('mqttPort').value), mode = Number($('mqttTlsMode').value);
  if (mode && port === 1883) $('mqttPort').value = 8883;
  if (!mode && port === 8883) $('mqttPort').value = 1883;
  updateTls();
});
function formValues(form, fields, numbers = []) {
  const values = {};
  for (const key of fields) values[key] = numbers.includes(key) ? Number(form.elements.namedItem(key).value) : form.elements.namedItem(key).value;
  return values;
}
function onForm(id, action) {
  $(id).addEventListener('submit', async event => {
    event.preventDefault();
    const form = event.currentTarget, submit = form.querySelector('[type=submit]');
    submit.disabled = true; messages(form, 'Выполняем…');
    try { await action(form); }
    catch (error) { messages(form, error.message, true); }
    finally { submit.disabled = false; }
  });
}
async function savePatch(form, patch) {
  const result = await api('/api/settings', {method: 'POST', body: patch});
  if (settingsData) Object.assign(settingsData, patch);
  messages(form, result.message);
}
onForm('mqtt-form', async form => {
  const patch = formValues(form, ['mqttHost','mqttPort','mqttUser','mqttBase','mqttIntervalS','haPrefix','mqttTlsMode','mqttFingerprint','ntpServer'], ['mqttPort','mqttIntervalS','mqttTlsMode']);
  if ($('mqtt-clear').checked) patch.mqttPass = '';
  else if ($('mqttPass').value) patch.mqttPass = $('mqttPass').value;
  await savePatch(form, patch);
  if ('mqttPass' in patch) { $('mqttPass').placeholder = patch.mqttPass ? 'Сохранён · не изменять' : 'Не задан'; delete settingsData.mqttPass; }
  $('mqttPass').value = ''; $('mqtt-clear').checked = false;
});
onForm('wifi-form', async form => {
  const result = await api('/api/wifi', {method: 'POST', body: {ssid: $('wifi-ssid').value, password: $('wifi-password').value, keepPassword: $('wifi-keep').checked}});
  $('wifi-password').value = ''; reconnectUntil = Date.now() + 60000;
  messages(form, result.message + ' Если связь потерялась, откройте новый IP-адрес или подключитесь к резервной сети устройства.');
});
$('scan-button').addEventListener('click', async () => {
  const button = $('scan-button'), form = $('wifi-form'); button.disabled = true;
  try {
    await api('/api/wifi/scan', {method: 'POST'}); messages(form, 'Ищем сети…');
    let result;
    for (let attempt = 0; attempt < 12; attempt++) {
      await new Promise(resolve => setTimeout(resolve, 1000));
      result = await api('/api/wifi/scan');
      if (!result.running) break;
    }
    if (result.running) throw new Error('Поиск занимает слишком много времени. Повторите попытку.');
    const list = $('network-list'); list.replaceChildren();
    const seen = new Set();
    result.networks.sort((a,b) => b.rssi-a.rssi).filter(network => network.ssid && !seen.has(network.ssid) && seen.add(network.ssid)).forEach(network => {
      const item = document.createElement('button'); item.type = 'button';
      const name = document.createElement('span'); name.textContent = network.ssid;
      const signal = document.createElement('small'); signal.textContent = `${network.secure ? '🔒 ' : ''}${network.rssi} dBm`;
      item.append(name, signal); item.addEventListener('click', () => { $('wifi-ssid').value = network.ssid; $('wifi-keep').checked = network.ssid === settingsData?.wifiSsid; updateWifiKeep(); list.hidden = true; $('wifi-password').focus(); });
      list.appendChild(item);
    });
    list.hidden = !list.childElementCount;
    messages(form, list.childElementCount ? 'Выберите сеть из списка.' : 'Сети не найдены. Можно ввести имя вручную.');
  } catch (error) { messages(form, error.message, true); }
  finally { button.disabled = false; }
});
function updateCalibration() {
  const values = ['adcFullScaleV','vZero','vFull','pFullMPa'].map(key => Number($(key).value));
  const [adc, zero, full, pressure] = values;
  const valid = values.every(Number.isFinite) && adc >= 1 && adc <= 10 && zero >= 0 && zero < adc && full <= 10 && full-zero > .1 && pressure > 0 && pressure <= 100;
  text('calibration-range', valid ? `0–${fmt((Math.min(adc,full)-zero)/(full-zero)*pressure*10)}` : '—');
  $('vFull').setCustomValidity(valid ? '' : 'Проверьте диапазон калибровки: полная шкала должна превышать нулевое напряжение более чем на 0,1 В.');
}
$('calibration-form').addEventListener('input', updateCalibration);
onForm('calibration-form', form => savePatch(form, formValues(form, ['adcFullScaleV','vZero','vFull','pFullMPa'], ['adcFullScaleV','vZero','vFull','pFullMPa'])));
onForm('name-form', form => savePatch(form, {deviceName: $('deviceName').value}));
onForm('password-form', async form => {
  if ($('portalPass').value !== $('portalPassConfirm').value) throw new Error('Пароли не совпадают.');
  await savePatch(form, {portalPass: $('portalPass').value});
  delete settingsData.portalPass;
  $('portalPass').value = ''; $('portalPassConfirm').value = '';
  setAuth(false);
});
onForm('ca-form', async form => {
  const file = $('ca-file').files[0];
  if (!file || !file.size || file.size > 4096) throw new Error('Выберите PEM-сертификат размером от 1 байта до 4 КБ.');
  const body = new FormData(); body.append('certificate', file);
  const result = await api('/api/ca', {method: 'POST', body, timeout: 30000});
  messages(form, result.message); $('ca-file').value = ''; poll();
});
$('ca-delete').addEventListener('click', async () => {
  if (!await confirmAction('Удалить сертификат?', 'Соединение MQTT в режиме проверки CA перестанет работать до загрузки нового сертификата.', 'Удалить')) return;
  const button = $('ca-delete'); button.disabled = true;
  try { const result = await api('/api/ca/delete', {method: 'POST'}); messages($('ca-form'), result.message); poll(); }
  catch (error) { messages($('ca-form'), error.message, true); }
  finally { button.disabled = false; }
});
function uploadFirmware(file) {
  return new Promise((resolve, reject) => {
    const request = new XMLHttpRequest();
    request.open('POST', '/api/firmware'); request.setRequestHeader('X-WPS-Request', '1'); request.timeout = 180000;
    request.upload.onprogress = event => { if (event.lengthComputable) $('upload-progress').value = Math.round(event.loaded / event.total * 100); };
    request.onerror = () => reject(new Error('Связь прервалась. После восстановления проверьте версию прошивки перед повторной загрузкой.'));
    request.ontimeout = () => reject(new Error('Время ожидания истекло. Проверьте версию после восстановления связи.'));
    request.onload = () => {
      try {
        const result = JSON.parse(request.responseText);
        if (request.status === 401) { setAuth(false); showLogin(); }
        if (request.status < 200 || request.status >= 300) reject(new Error(result.error || 'Обновление не выполнено.'));
        else resolve(result);
      } catch (_) { reject(new Error('Не удалось прочитать ответ. Проверьте версию прошивки после перезагрузки.')); }
    };
    const body = new FormData(); body.append('firmware', file); request.send(body);
  });
}
onForm('firmware-form', async form => {
  const file = $('firmware-file').files[0];
  if (!file || !file.size || !file.name.toLowerCase().endsWith('.bin')) throw new Error('Выберите непустой файл firmware.bin.');
  if (!await confirmAction('Обновить прошивку?', `Будет загружен файл ${file.name}. После записи устройство перезагрузится.`, 'Обновить')) { messages(form, 'Обновление отменено.'); return; }
  uploading = true; clearTimeout(pollTimer); $('upload-progress').hidden = false; $('upload-progress').value = 0;
  messages(form, 'Загружаем и проверяем прошивку…');
  try {
    const result = await uploadFirmware(file); reconnectUntil = Date.now() + 60000;
    messages(form, result.message); $('firmware-file').value = '';
  } finally { uploading = false; $('upload-progress').hidden = true; pollTimer = setTimeout(poll, 3000); }
});
$('restart-button').addEventListener('click', async () => {
  if (!await confirmAction('Перезагрузить устройство?', 'Измерения возобновятся после запуска. Сохранённые настройки останутся на устройстве.', 'Перезагрузить')) return;
  const button = $('restart-button'); button.disabled = true;
  try { const result = await api('/api/restart', {method: 'POST'}); reconnectUntil = Date.now() + 60000; text('restart-message', result.message); $('restart-message').classList.remove('error'); }
  catch (error) { text('restart-message', error.message); $('restart-message').classList.add('error'); }
  finally { button.disabled = false; }
});
poll();

