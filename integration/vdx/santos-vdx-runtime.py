#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verified persistent r7 bootstrap and future-boot-only rollback; no live unload."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import sys
import tempfile
import time
import uuid

BASE = Path('/opt/santos-vdx-r7')
OWNER = 'santos-vdx-r7-persistent-20261002'
LOADER = Path('/etc/sv/santos-pvr/run.highmem-p11')
RUNTIME = Path('/run/santos-vdx-r7')
READY = Path('/run/santos-pvr.ready')
ATTEMPTED = Path('/run/santos-pvr-init-attempted')


class RuntimeErrorState(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise RuntimeErrorState(message)


def digest(path):
    item = Path(path)
    require(item.is_file() and not item.is_symlink(), 'not a regular owned file: ' + str(item))
    return hashlib.sha256(item.read_bytes()).hexdigest()


def write_json(path, value):
    path = Path(path)
    require(not path.is_symlink(), 'refusing symlink receipt/state')
    with tempfile.NamedTemporaryFile(mode='w', dir=path.parent,
                                     prefix=path.name + '.new-', delete=False) as stream:
        temporary = Path(stream.name)
        os.chmod(temporary, 0o600)
        json.dump(value, stream, indent=2)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)
    descriptor = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def read_json(path):
    item = Path(path)
    require(item.is_file() and not item.is_symlink(), 'unknown manifest/state file')
    value = json.loads(item.read_text())
    require(isinstance(value, dict), 'manifest/state must be a JSON object')
    return value


def boot_id():
    value = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    require(str(uuid.UUID(value)) == value, 'invalid boot identity')
    return value


def note_ids(raw):
    result = []
    position = 0
    while position + 12 <= len(raw):
        n, d, kind = struct.unpack_from('<III', raw, position)
        offset = position + 12 + ((n + 3) & ~3)
        require(offset + d <= len(raw), 'truncated ELF note')
        if kind == 3 and raw[position + 12:position + 12 + n].rstrip(b'\0') == b'GNU':
            result.append(raw[offset:offset + d].hex())
        position = offset + ((d + 3) & ~3)
    return result


def loaded_ids(names):
    result = {}
    for name in names:
        root = Path('/sys/module') / name
        if not root.exists():
            continue
        ids = note_ids((root / 'notes/.note.gnu.build-id').read_bytes())
        require(len(ids) == 1, 'ambiguous loaded module: ' + name)
        result[name] = ids[0]
    return result


def kernel_ids():
    return note_ids(Path('/sys/kernel/notes').read_bytes())


def validate_loaded(expected, actual):
    for name, value in actual.items():
        require(expected.get(name) == value, 'mixed/unknown loaded module: ' + name)


def verify_bundle(manifest):
    require(manifest.get('owner') == OWNER and manifest.get('format') == 1, 'unknown deployment owner/format')
    for name, wanted in manifest['files'].items():
        relative = Path(name)
        require(not relative.is_absolute() and '..' not in relative.parts, 'unsafe bundle path')
        require(digest(BASE / relative) == wanted, 'deployment hash differs: ' + name)
    require(digest(BASE / 'baseline/run.highmem-p11') == manifest['baseline_loader_sha256'],
            'original loader backup differs')


def validate_selection(selection):
    require(selection.get('owner') == OWNER and selection.get('format') == 1, 'unknown selection owner')
    require(selection.get('selected') in ['staged', 'r7', 'baseline'], 'unknown selected profile')


def replace_loader(content, mode):
    require(LOADER.is_file() and not LOADER.is_symlink(), 'unknown live loader file')
    uid, gid = LOADER.stat().st_uid, LOADER.stat().st_gid
    with tempfile.NamedTemporaryFile(dir=LOADER.parent, prefix='run.highmem-p11.r7-new-', delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(content)
        stream.flush()
        os.fsync(stream.fileno())
    os.chmod(temporary, mode)
    os.chown(temporary, uid, gid)
    os.replace(temporary, LOADER)
    descriptor = os.open(LOADER.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def plan_boot(previous, current_boot, actual, expected, ready, attempted):
    validate_loaded(expected, actual)
    if ready:
        require(actual == expected, 'READY has incomplete module identities')
    if previous is not None:
        require(previous.get('owner') == OWNER and previous.get('boot_id') == current_boot
                and previous.get('profile') == 'r7', 'unknown/stale bootstrap receipt')
        return 'hold'
    if ready or attempted:
        return 'hold'
    require(not actual, 'unverified partial bootstrap; refusing second init')
    return 'bootstrap'


def owned_runtime_directory(path):
    return (path.is_dir() and not path.is_symlink() and path.stat().st_uid == 0
            and stat.S_IMODE(path.stat().st_mode) == 0o700)


def owned_deployment_root(path):
    return (path.is_dir() and not path.is_symlink() and path.stat().st_uid == 0
            and not stat.S_IMODE(path.stat().st_mode) & 0o022)


def claim_boot(current_boot, actual, manifest):
    if RUNTIME.exists():
        require(owned_runtime_directory(RUNTIME), 'unknown runtime directory')
        marker = RUNTIME / 'owner.json'
        require(marker.is_file(), 'unowned runtime directory')
        require(read_json(marker) == {'owner': OWNER, 'format': 1}, 'unknown runtime owner')
    else:
        RUNTIME.mkdir(mode=0o700)
        write_json(RUNTIME / 'owner.json', {'owner': OWNER, 'format': 1})
    path = RUNTIME / 'boot.json'
    previous = read_json(path) if path.exists() or path.is_symlink() else None
    action = plan_boot(previous, current_boot, actual, manifest['loaded_ids'], READY.exists(), ATTEMPTED.exists())
    if previous is None:
        # The global runner still owns its Services init-attempted marker.
        # Our durable claim serializes entry before it reaches that marker.
        write_json(path, {'owner': OWNER, 'format': 1, 'boot_id': current_boot,
                          'profile': 'r7', 'action': action, 'bootstrap_claimed': action == 'bootstrap',
                          'loaded_ids_before': actual, 'time_utc': time.time()})
    return action


def promote(manifest, selection, current_boot, actual):
    verify_bundle(manifest)
    validate_selection(selection)
    require(selection['selected'] == 'staged', 'deployment already selected; refusing implicit change')
    require(current_boot == manifest['promotion_boot'], 'promotion boot differs')
    require(digest(LOADER) == manifest['baseline_loader_sha256'], 'original live loader changed')
    require(actual == manifest['loaded_ids'] and READY.exists(), 'tested r7 pair is not fully live/ready')
    require(not Path('/run/santos_hwc_fault').exists(), 'HWC fault precondition')
    for name, wanted in manifest['promotion_protected_files'].items():
        require(hashlib.sha256(Path(name).read_bytes()).hexdigest() == wanted, 'protected file changed: ' + name)
    for name, wanted in manifest['original_directory_files'].items():
        require(digest(name) == wanted, 'original module directory changed')
    write_json(BASE / 'selection.json', {'owner': OWNER, 'format': 1, 'selected': 'r7',
                                       'promoted_boot': current_boot, 'time_utc': time.time()})
    replace_loader(manifest['launcher'].encode(), manifest['loader_mode'])
    require(digest(LOADER) == manifest['launcher_sha256'], 'persistent loader readback differs')
    result = {'owner': OWNER, 'boot_id': current_boot, 'selected': 'r7',
              'loader_sha256': digest(LOADER), 'loaded_ids_unchanged': actual,
              'current_modules_reloaded_or_rebooted': False, 'persistent_coldboot_retested': False}
    write_json(BASE / 'promotion-receipt.json', result)
    return result


def rollback(manifest, selection, current_boot, actual):
    # Recovery must not depend on a still-valid new .ko/kernel/helper hash.
    # Own manifest, live launcher and retained original loader identities suffice;
    # no uncertain DMA/module state is overwritten or forcibly unloaded.
    require(manifest.get('owner') == OWNER and manifest.get('format') == 1, 'unknown rollback owner')
    validate_selection(selection)
    require(digest(LOADER) in [manifest['launcher_sha256'], manifest['baseline_loader_sha256']],
            'not our loader; refusing rollback overwrite')
    backup = BASE / 'baseline/run.highmem-p11'
    require(digest(backup) == manifest['baseline_loader_sha256'], 'original loader backup differs')
    write_json(BASE / 'selection.json', {'owner': OWNER, 'format': 1, 'selected': 'baseline',
                                       'restored_boot': current_boot, 'time_utc': time.time()})
    replace_loader(backup.read_bytes(), manifest['loader_mode'])
    require(digest(LOADER) == manifest['baseline_loader_sha256'], 'original loader restore readback differs')
    result = {'owner': OWNER, 'boot_id': current_boot, 'selected': 'baseline-for-future-boots',
              'loader_sha256': digest(LOADER), 'current_loaded_ids_unchanged': actual,
              'current_modules_reloaded_unloaded_or_rebooted': False}
    write_json(BASE / 'rollback-receipt.json', result)
    return result


def main():
    require(os.geteuid() == 0, 'root required')
    require(owned_deployment_root(BASE), 'unknown deployment root')
    operation = sys.argv[1] if len(sys.argv) == 2 else '--run'
    require(operation in ['--run', '--verify', '--promote', '--rollback'], 'unknown operation')
    manifest = read_json(BASE / 'manifest.json')
    lock_fd = os.open(BASE / 'runtime.lock', os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    with os.fdopen(lock_fd, 'r+') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        selection = read_json(BASE / 'selection.json')
        current_boot = boot_id()
        actual = loaded_ids(manifest['loaded_ids'])
        if operation == '--rollback':
            print(json.dumps(rollback(manifest, selection, current_boot, actual), indent=2))
            return
        verify_bundle(manifest)
        validate_selection(selection)
        require(manifest['kernel_build_id'] in kernel_ids(),
                'unsupported kernel; no bootstrap action')
        validate_loaded(manifest['loaded_ids'], actual)
        if operation == '--promote':
            print(json.dumps(promote(manifest, selection, current_boot, actual), indent=2))
            return
        require(digest(LOADER) == manifest['launcher_sha256'] and selection['selected'] == 'r7',
                'persistent r7 selection is not active')
        if operation == '--verify':
            print(json.dumps({'owner': OWNER, 'boot_id': current_boot, 'selected': 'r7',
                              'verified_bundle': True, 'loader_sha256': digest(LOADER),
                              'loaded_ids': actual, 'all_expected_modules_loaded': actual == manifest['loaded_ids'],
                              'ready': READY.exists(), 'bootstrap_or_hardware_action': False}, indent=2))
            return
        action = claim_boot(current_boot, actual, manifest)
        print('santos-vdx-r7: boot=' + current_boot + ' action=' + action, flush=True)
    if action == 'hold':
        os.execv('/bin/sleep', ['/bin/sleep', 'infinity'])
    os.execv('/bin/sh', ['/bin/sh', str(BASE / 'run.modules')])


if __name__ == '__main__':
    try:
        main()
    except (RuntimeErrorState, OSError, ValueError, KeyError) as error:
        print('santos-vdx-r7: fail-closed: ' + str(error), file=sys.stderr, flush=True)
        raise SystemExit(1)
