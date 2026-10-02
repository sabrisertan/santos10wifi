#!/usr/bin/env python3
"""Own one leased package CPUFreq driver and the built-in performance governor.

No battery/brightness policy or thermal-MSR writes. A lost daemon lease causes
kernel unregister+restore. Only a pinned module with this service's receipt may
be recovered/unloaded; uncertain control ownership remains quarantined.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import signal
import struct
import subprocess
import threading
import time
import uuid

OWNER = 'santos-cpufreq-performance-20261002'
MODULE_NAME = 'santos_sfi_cpufreq'
SYS = Path('/sys/module') / MODULE_NAME
BASE = Path(__file__).resolve().parent
STATE_DIR = Path('/run/santos-cpufreq')
stop = threading.Event()


def log(message):
    print('SANTOS_CPUFREQ ' + message, flush=True)


def proc_identity(pid):
    proc = Path('/proc') / str(pid)
    stat = (proc / 'stat').read_text()
    return {'pid': pid, 'exe': os.readlink(proc / 'exe'),
            'starttime': stat[stat.rfind(')') + 2:].split()[19]}


def boot_id():
    return Path('/proc/sys/kernel/random/boot_id').read_text().strip()


def build_ids(raw):
    result = []
    offset = 0
    while offset + 12 <= len(raw):
        namesz, descsz, kind = struct.unpack_from('<III', raw, offset)
        offset += 12
        name = raw[offset:offset + namesz].rstrip(b'\0')
        offset += (namesz + 3) & ~3
        value = raw[offset:offset + descsz]
        offset += (descsz + 3) & ~3
        if name == b'GNU' and kind == 3:
            result.append(value.hex())
    return result


def module_matches(config):
    return SYS.exists() and build_ids((SYS / 'notes/.note.gnu.build-id').read_bytes()) == [config['module_build_id']]


def snapshot(config, instance=None):
    if not module_matches(config):
        raise RuntimeError('loaded module identity mismatch')
    if instance is not None and (SYS / 'parameters/owner_token').read_text().strip() != instance:
        raise RuntimeError('loaded module instance changed')
    return json.loads((SYS / 'parameters/snapshot').read_text())


def validate_snapshot(data, running):
    if data.get('domain') != 'package' or data.get('last_error') or data.get('restore_error'):
        raise RuntimeError('package driver health error')
    if len(data.get('cpus', [])) != 4:
        raise RuntimeError('unexpected CPU topology')
    if running and (not data.get('registered') or data.get('expired') or data.get('released')):
        raise RuntimeError('driver no longer active; never revive an expired lease')
    for cpu in data['cpus']:
        if cpu['errors'] != [0, 0, 0, 0, 0, 0]:
            raise RuntimeError('MSR read health error')
        thermal = int(cpu['thermal'], 16)
        if not thermal & (1 << 31) or thermal & ((1 << 0) | (1 << 10)):
            raise RuntimeError('invalid or throttled DTS; keep native thermal protection')


def restored_snapshot(data):
    return (not data.get('registered') and data.get('released') and data.get('restored')
            and not data.get('restore_error')
            and all(int(cpu['ctl'], 16) == int(cpu['original_ctl'], 16)
                    and cpu['misc'] == cpu['initial_misc'] for cpu in data['cpus']))


def save(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.chmod(0o600)
    temporary.replace(path)


def ensure_state_directory():
    try:
        STATE_DIR.mkdir(mode=0o700)
    except FileExistsError:
        marker = STATE_DIR / 'owner'
        if not marker.is_file() or marker.read_text().strip() != OWNER:
            raise RuntimeError('existing runtime directory is not owned by this service')
    else:
        marker = STATE_DIR / 'owner'
        marker.write_text(OWNER + '\n')
        marker.chmod(0o600)


def command(args):
    result = subprocess.run(args, capture_output=True, text=True, timeout=15)
    if result.returncode:
        raise RuntimeError('command failed: ' + repr(args) + ': ' + result.stderr.strip())


def release_owned(config, receipt, reason):
    if receipt.get('owner') != OWNER or receipt.get('boot_id') != boot_id():
        raise RuntimeError('not this service/boot ownership')
    if receipt.get('module_build_id') != config['module_build_id']:
        raise RuntimeError('receipt module identity mismatch')
    if not SYS.exists():
        if receipt.get('phase') != 'loading':
            raise RuntimeError('owned running module unexpectedly absent; controls unverified')
        return {'reason': reason, 'candidate_absent': True, 'already_absent': True,
                'controls_restored': None, 'no_published_module': True}
    if (SYS / 'parameters/owner_token').read_text().strip() != receipt.get('instance'):
        raise RuntimeError('module instance is not owned; same build-id is insufficient')
    before = snapshot(config, receipt['instance'])
    # Kernel release unregisters policy before restoring only tracked controls.
    (SYS / 'parameters/release').write_text(receipt['instance'])
    after = snapshot(config, receipt['instance'])
    if not restored_snapshot(after):
        raise RuntimeError('restore unverified: quarantine, never force unload')
    if list(Path('/sys/devices/system/cpu/cpufreq').glob('policy*')):
        raise RuntimeError('unexpected policy remains, do not unload')
    command(['rmmod', MODULE_NAME])
    return {'reason': reason, 'before': before, 'after': after,
            'candidate_absent': not SYS.exists(), 'controls_restored': True}


def policy_path():
    policies = list(Path('/sys/devices/system/cpu/cpufreq').glob('policy*'))
    if len(policies) != 1 or policies[0].name != 'policy0':
        raise RuntimeError('expected one package policy0')
    return policies[0]


def validate_policy(path, profile):
    checks = {'scaling_driver': 'santos-sfi', 'scaling_governor': profile['governor'],
              'scaling_min_freq': str(profile['min_khz']),
              'scaling_max_freq': str(profile['max_khz'])}
    for name, expected in checks.items():
        if (path / name).read_text().strip() != expected:
            raise RuntimeError('policy ownership/limits changed: ' + name)
    if (path / 'related_cpus').read_text().split() != ['0', '1', '2', '3']:
        raise RuntimeError('CPU0/SMT coverage lost')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--duration', type=float, default=None,
                        help='bounded test run, seconds; normal service renews until stopped')
    args = parser.parse_args()
    if args.duration is not None and not 2 <= args.duration <= 180:
        raise RuntimeError('invalid bounded duration')
    config = json.loads((BASE / 'manifest.json').read_text())
    profile = config['profile']
    if (profile['governor'] != 'performance' or profile['min_khz'] != 800000
            or profile['max_khz'] != 1600000 or not 5000 <= profile['lease_ms'] <= 120000
            or not 1 <= profile['heartbeat_seconds'] < profile['lease_ms'] / 3000):
        raise RuntimeError('unsupported aggressive profile')
    for name, digest in config['files'].items():
        if Path(name).name != name or hashlib.sha256((BASE / name).read_bytes()).hexdigest() != digest:
            raise RuntimeError('deployment hash mismatch: ' + name)
    if build_ids(Path('/sys/kernel/notes').read_bytes()) != [config['kernel_build_id']]:
        log('SKIP incompatible kernel; no module/clock action')
        while not stop.wait(60):
            pass
        return
    ensure_state_directory()
    with (STATE_DIR / 'manager.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        state_path = STATE_DIR / 'state.json'
        old = json.loads(state_path.read_text()) if state_path.exists() else None
        if old is not None and old.get('owner') != OWNER:
            raise RuntimeError('existing receipt is not owned; never overwrite')
        if (old is not None and old.get('boot_id') == boot_id() and
                old.get('phase') != 'stopped' and not SYS.exists()):
            raise RuntimeError('previous owned module vanished without verified cleanup')
        if SYS.exists():
            if not state_path.exists():
                raise RuntimeError('unowned existing CPUFreq module; do not replace')
            try:
                old_still_alive = proc_identity(old['process']['pid']) == old['process']
            except (OSError, KeyError):
                old_still_alive = False
            if old_still_alive:
                raise RuntimeError('existing module manager identity alive')
            recovery = release_owned(config, old, 'dead owned manager recovery')
            save(STATE_DIR / 'recovery.json', recovery)
        if list(Path('/sys/devices/system/cpu/cpufreq').glob('policy*')):
            raise RuntimeError('another CPUFreq driver owns policies')
        if Path('/sys/devices/system/cpu/online').read_text().strip() != '0-3':
            raise RuntimeError('four online CPUs required; no automatic hotplug')
        receipt = {'owner': OWNER, 'instance': str(uuid.uuid4()), 'boot_id': boot_id(),
                   'process': proc_identity(os.getpid()), 'module_build_id': config['module_build_id'],
                   'phase': 'loading', 'profile': profile, 'kernel_build_id': config['kernel_build_id']}
        save(state_path, receipt)
        attempted = False
        try:
            attempted = True
            command(['insmod', str(BASE / 'santos-sfi-cpufreq.ko'), 'enable=1',
                     'lease_ms=' + str(profile['lease_ms']), 'owner_token=' + receipt['instance']])
            data = snapshot(config, receipt['instance'])
            validate_snapshot(data, True)
            receipt['initial'] = data
            path = policy_path()
            if profile['governor'] not in (path / 'scaling_available_governors').read_text().split():
                raise RuntimeError('requested built-in governor absent')
            (path / 'scaling_max_freq').write_text(str(profile['max_khz']))
            (path / 'scaling_min_freq').write_text(str(profile['min_khz']))
            (path / 'scaling_governor').write_text(profile['governor'])
            validate_policy(path, profile)
            validate_snapshot(snapshot(config, receipt['instance']), True)
            receipt['phase'] = 'running'; receipt['started_monotonic'] = time.monotonic()
            save(state_path, receipt)
            log('READY performance 1600000kHz package CPUs0-3; battery/brightness caps absent')
            start = time.monotonic()
            while not stop.is_set() and (args.duration is None or time.monotonic() - start < args.duration):
                if Path('/sys/devices/system/cpu/online').read_text().strip() != '0-3':
                    raise RuntimeError('online mask changed; fail closed')
                validate_policy(path, profile)
                data = snapshot(config, receipt['instance']); validate_snapshot(data, True)
                (SYS / 'parameters/keepalive').write_text(receipt['instance'])
                receipt['last_snapshot'] = data; receipt['heartbeat_monotonic'] = time.monotonic()
                save(state_path, receipt)
                wait = profile['heartbeat_seconds']
                if args.duration is not None:
                    wait = min(wait, max(0, args.duration - (time.monotonic() - start)))
                stop.wait(wait)
        finally:
            if attempted:
                cleanup = release_owned(config, receipt, 'manager shutdown/exception')
                receipt.update(phase='stopped', cleanup=cleanup)
                save(state_path, receipt)
                if cleanup.get('controls_restored'):
                    log('STOPPED original controls restored; candidate/policy removed')
                else:
                    log('STOPPED no module published; no clock restoration verdict')


def signal_stop(signum, frame):
    del signum, frame
    stop.set()


if __name__ == '__main__':
    for sig in [signal.SIGTERM, signal.SIGINT]:
        signal.signal(sig, signal_stop)
    try:
        main()
    except BaseException as error:
        log('ERROR ' + repr(error))
        # A recovery/ownership failure is not a reason to keep writing clocks.
        raise
