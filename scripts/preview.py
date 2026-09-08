"""Local UI preview only. Simulated device state never enters the firmware."""
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
from pathlib import Path
from urllib.parse import urlsplit
import argparse
import copy
import json
import time

ROOT = Path(__file__).resolve().parents[1]
SETTINGS = dict(mqttHost='mqtt.home.local', mqttPort=8883, mqttUser='water', mqttBase='watersensor/wps_abc123',
                mqttIntervalS=10, haPrefix='homeassistant', deviceName='Датчик в котельной', mqttTlsMode=3,
                mqttFingerprint='', ntpServer='pool.ntp.org', adcFullScaleV=3.2, vZero=0.5, vFull=4.5,
                pFullMPa=0.5, wifiSsid='Home Network', mqttPasswordSet=True)
START = time.monotonic()


class Preview(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(ROOT / 'web'), **kwargs)

    def log_message(self, *_):
        pass

    def reply(self, data, status=200, cookie=None):
        body = json.dumps(data, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(body)))
        if cookie:
            self.send_header('Set-Cookie', cookie)
        self.end_headers()
        self.wfile.write(body)

    def authorized(self):
        return 'wps_session=preview' in self.headers.get('Cookie', '')

    def do_GET(self):
        path = urlsplit(self.path).path
        if path == '/api/status':
            reading = dict(bar=2.47, mpa=0.247, volt=2.477, raw=792, raw_instant=793,
                           rssi=-58, over=False, fault=False, heap=36480, age_ms=12, stale=False)
            self.reply(dict(demo=True, reading=reading, name=SETTINGS['deviceName'], id='wps_abc123', version='3.0.0',
                            uptime_s=int(time.monotonic()-START)+86400, max_bar=3.375, heap_block=31000,
                            authenticated=self.authorized(), wifi=dict(connected=True, ssid=SETTINGS['wifiSsid'], ip='192.168.1.42',
                            ap=False, ap_name='WaterSensor-abc123', ap_ip='192.168.4.1'),
                            mqtt=dict(enabled=True, connected=True, tls=True, mode=3, error=''),
                            ca=dict(present=True, size=1810, error=''), tls_error='', storage_error=''))
        elif path == '/api/settings':
            self.reply(SETTINGS if self.authorized() else dict(error='Войдите в интерфейс.'), 200 if self.authorized() else 401)
        elif path == '/api/wifi/scan':
            self.reply(dict(running=False, networks=[dict(ssid='Home Network', rssi=-58, secure=True), dict(ssid='Guest', rssi=-72, secure=False)]))
        elif path.startswith('/api/'):
            self.reply(dict(error='Неизвестный API'), 404)
        else:
            super().do_GET()

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        path = urlsplit(self.path).path
        if self.headers.get('X-WPS-Request') != '1':
            return self.reply(dict(error='Требуется заголовок интерфейса.'), 403)
        data = json.loads(raw) if self.headers.get('Content-Type', '').startswith('application/json') else {}
        if path == '/api/login':
            if data.get('password') != 'preview-access123':
                return self.reply(dict(error='Неверный пароль интерфейса.'), 401)
            return self.reply(dict(message='Вход выполнен.'), cookie='wps_session=preview; Path=/; HttpOnly; SameSite=Strict')
        if not self.authorized():
            return self.reply(dict(error='Войдите в интерфейс.'), 401)
        if path == '/api/logout':
            return self.reply(dict(message='Вы вышли.'), cookie='wps_session=; Path=/; Max-Age=0')
        if path == '/api/settings':
            update = copy.copy(data)
            update.pop('portalPass', None)
            if 'mqttPass' in update:
                update['mqttPasswordSet'] = bool(update.pop('mqttPass'))
            SETTINGS.update(update)
        self.reply(dict(message='Демонстрация: действие выполнено. Физическое устройство не изменено.'))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8765)
    args = parser.parse_args()
    print(f'Preview http://127.0.0.1:{args.port}/ — password: preview-access123', flush=True)
    ThreadingHTTPServer(('127.0.0.1', args.port), Preview).serve_forever()
