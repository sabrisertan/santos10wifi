#!/usr/bin/python3
"""Santos runit power policy. JSON-lines API; all deadlines use monotonic time."""
import argparse
import glob
import json
import os
from pathlib import Path
import select
import signal
import socket
import struct
import tempfile
import time
import uuid

DEFAULTS = dict(screen_off_seconds=300, brightness=128)
RUNTIME = Path('/run/santos-power')
CONFIG = Path('/etc/santos-power.json')
# Sensors must not count as human activity. No EVIOCGRAB: Qt still receives input.
INPUT_NAMES = {'sec_key', 'sec_touchscreen', 'max77693-muic dockkey',
               'sii9234_rcp', 'mid_powerbtn'}
EVENT = struct.Struct('@llHHi')


def validate(config):
    if set(config) != set(DEFAULTS):
        raise ValueError('unknown/missing setting')
    for key in ('screen_off_seconds',):
        value = config[key]
        if type(value) is not int or not 0 <= value <= 86400:
            raise ValueError(key + ' must be an integer from 0 to 86400 (0 disables)')
    if type(config['brightness']) is not int or not 1 <= config['brightness'] <= 255:
        raise ValueError('brightness must be an integer from 1 to 255')
    return config


def atomic_config(config):
    fd, path = tempfile.mkstemp(prefix='.santos-power-', dir=CONFIG.parent)
    try:
        with os.fdopen(fd, 'w') as out:
            json.dump(config, out, indent=2)
            out.write('\n')
            out.flush()
            os.fsync(out.fileno())
        os.replace(path, CONFIG)
    finally:
        if os.path.exists(path):
            os.unlink(path)


def display(command):
    path = str(RUNTIME / ('request-' + uuid.uuid4().hex))
    with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as conn:
        try:
            conn.bind(path)
            conn.settimeout(2)
            conn.sendto(command.encode(), str(RUNTIME / 'display.sock'))
            result = json.loads(conn.recv(4096))
            if not result['ok']:
                raise RuntimeError('display control failed: ' + str(result))
            return result
        finally:
            if os.path.exists(path):
                os.unlink(path)


def audio_running():
    # Works for ordinary ALSA applications, including the installed mpv launcher.
    for path in glob.glob('/proc/asound/card*/pcm*p/sub*/status'):
        try:
            if 'state: RUNNING' in Path(path).read_text():
                return True
        except OSError:
            pass
    return False


def due(config, idle, on, blocked):
    if (on and config['screen_off_seconds'] and idle >= config['screen_off_seconds']
            and 'screen' not in blocked):
        return 'off'
    return None


class Power:
    def __init__(self):
        saved = json.loads(CONFIG.read_text()) if CONFIG.exists() else {}
        # Migrate the never-enabled suspend settings from the initial prototype.
        saved.pop('suspend_after_seconds', None)
        saved.pop('suspend_enabled', None)
        self.config = validate(DEFAULTS | saved)
        self.last_activity = time.monotonic()
        self.leases = {}
        self.inputs = {}
        self.on = True
        self.connected = False
        self.error = None
        self.last_scan = 0
        self.last_retry = 0
        self.media_active = False
        self.last_power_key = -1.0

    def inhibitors(self):
        now = time.monotonic()
        self.leases = {k: v for k, v in self.leases.items() if v['expires'] > now}
        items = list(self.leases.values())
        if self.media_active:
            items = items + [dict(reason='ALSA playback', scopes=['screen'])]
        return items

    def set_display(self, command):
        reply = display(command)
        self.on = reply['display'] == 'on'
        if command == 'on':
            Path('/sys/class/backlight/panel/brightness').write_text(str(self.config['brightness']))
        self.connected = True
        self.error = None
        return reply

    def activity(self):
        self.last_activity = time.monotonic()
        if not self.on:
            self.set_display('on')

    def request(self, request):
        if not isinstance(request, dict):
            raise ValueError('request must be a JSON object')
        method = request.get('method', 'status')
        if method == 'status':
            return dict(config=self.config, display='on' if self.on else 'off',
                        compositor_connected=self.connected, last_error=self.error,
                        idle_seconds=round(time.monotonic() - self.last_activity, 1),
                        inhibitors=self.inhibitors())
        if method == 'configure':
            updated = validate(self.config | request['settings'])
            if self.on and updated['brightness'] != self.config['brightness']:
                Path('/sys/class/backlight/panel/brightness').write_text(str(updated['brightness']))
            atomic_config(updated)
            self.config = updated
            self.last_activity = time.monotonic()
            return self.config
        if method == 'screen':
            if request['state'] not in ('on', 'off'):
                raise ValueError('state must be on or off')
            self.last_activity = time.monotonic()
            return self.set_display(request['state'])
        if method == 'activity':
            self.activity()
            return {}
        if method == 'inhibit':
            scopes = request.get('scopes', ['screen'])
            seconds = request.get('seconds', 60)
            reason = request.get('reason', 'application')
            if (not isinstance(scopes, list) or not scopes
                    or any(s != 'screen' for s in scopes)
                    or type(seconds) is not int or not 1 <= seconds <= 3600
                    or not isinstance(reason, str) or len(reason) > 200):
                raise ValueError('invalid inhibitor scopes, seconds, or reason')
            token = request.get('token')
            if token is not None and token not in self.leases:
                raise ValueError('unknown inhibitor token')
            token = token or uuid.uuid4().hex
            self.leases[token] = dict(scopes=scopes, reason=reason,
                                      expires=time.monotonic() + seconds)
            if 'screen' in scopes:
                self.activity()
            return dict(token=token)
        if method == 'release':
            self.leases.pop(request['token'], None)
            self.last_activity = time.monotonic()
            return {}
        raise ValueError('unknown method')

    def scan(self):
        for path in glob.glob('/sys/class/input/event*/device/name'):
            name = Path(path).read_text().strip()
            event = Path(path).parent.parent.name
            node = '/dev/input/' + event
            if name in INPUT_NAMES and node not in self.inputs.values():
                try:
                    fd = os.open(node, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
                    self.inputs[fd] = node
                except OSError:
                    pass

    def tick(self):
        now = time.monotonic()
        if now - self.last_scan >= 5:
            self.scan()
            self.last_scan = now
        self.media_active = audio_running()
        items = self.inhibitors()
        blocked = {s for item in items for s in item['scopes']}
        if 'screen' in blocked:
            # Do not switch off immediately when a film ends or is paused.
            self.last_activity = now
        if now - self.last_retry < 5:
            return
        try:
            self.set_display('status')
            action = due(self.config, now - self.last_activity, self.on, blocked)
            if action == 'off':
                self.set_display('off')
        except (OSError, ValueError, RuntimeError) as error:
            self.connected = False
            message = str(error)
            if message != self.error:
                print('power:', message, flush=True)
            self.error = message
            self.last_retry = now

    def run(self):
        RUNTIME.mkdir(mode=0o700, parents=True, exist_ok=True)
        os.chmod(RUNTIME, 0o700)
        path = RUNTIME / 'control.sock'
        path.unlink(missing_ok=True)
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
            server.bind(str(path))
            os.chmod(path, 0o600)
            server.listen(8)
            self.scan()
            while True:
                ready, _, _ = select.select([server, *self.inputs], [], [], 1)
                for source in ready:
                    try:
                        if source is server:
                            conn, _ = server.accept()
                            with conn:
                                conn.settimeout(0.25)
                                data = b''
                                while b'\n' not in data and len(data) <= 8192:
                                    block = conn.recv(8192)
                                    if not block:
                                        break
                                    data += block
                                try:
                                    if len(data) > 8192:
                                        raise ValueError('request too large')
                                    result = dict(ok=True, result=self.request(json.loads(data)))
                                except (ValueError, KeyError, TypeError, OSError, RuntimeError) as error:
                                    result = dict(ok=False, error=str(error))
                                conn.sendall((json.dumps(result) + '\n').encode())
                        else:
                            data = os.read(source, EVENT.size * 64)
                            if not data:
                                os.close(source)
                                del self.inputs[source]
                                continue
                            for offset in range(0, len(data) - EVENT.size + 1, EVENT.size):
                                _, _, kind, code, value = EVENT.unpack_from(data, offset)
                                if kind == 1 and code == 116 and value == 1:
                                    now = time.monotonic()
                                    if now - self.last_power_key >= 0.35:
                                        self.last_power_key = now
                                        self.last_activity = now
                                        self.set_display('off' if self.on else 'on')
                                elif kind in (1, 2, 3) and not (kind == 1 and code == 116):
                                    self.activity()
                    except (OSError, ValueError, RuntimeError) as error:
                        self.error = str(error)
                        if isinstance(source, int) and source in self.inputs and isinstance(error, OSError) and error.errno == 19:
                            os.close(source)
                            del self.inputs[source]
                self.tick()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--daemon', action='store_true')
    parser.add_argument('--timeout', type=float, default=8)
    parser.add_argument('request', nargs='?', default='{"method":"status"}',
                        help='one JSON object; default: status')
    args = parser.parse_args()
    if args.daemon:
        power = Power()
        def stop(*_):
            try:
                power.set_display('on')
            except (OSError, ValueError, RuntimeError):
                pass
            raise SystemExit(0)
        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGINT, stop)
        power.run()
    else:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as conn:
            conn.settimeout(args.timeout)
            conn.connect(str(RUNTIME / 'control.sock'))
            conn.sendall((args.request + '\n').encode())
            data = b''
            while b'\n' not in data:
                part = conn.recv(8192)
                if not part:
                    raise RuntimeError('daemon disconnected')
                data += part
        reply = json.loads(data)
        print(json.dumps(reply, indent=2))
        raise SystemExit(0 if reply['ok'] else 1)


if __name__ == '__main__':
    main()
