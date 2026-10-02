#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Create a NEW local service bundle from matching builds; never access a device."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def file_build_id(path):
    result = subprocess.run(['readelf', '-n', str(path)], capture_output=True, text=True, check=True)
    ids = re.findall(r'Build ID: ([0-9a-f]+)', result.stdout)
    if len(ids) != 1:
        raise ValueError('expected one GNU build-id in ' + str(path))
    return ids[0]


def prepare(kernel, module, destination):
    kernel = kernel.resolve()
    module = module.resolve()
    destination = destination.resolve()
    if destination.exists() or destination.is_symlink():
        raise ValueError('destination already exists; use a new bundle directory')
    if not module.is_file() or module.name != 'santos-sfi-cpufreq.ko':
        raise ValueError('expected the newly built santos-sfi-cpufreq.ko')
    config = (kernel / '.config').read_text().splitlines()
    for required in ['CONFIG_MODULES=y', 'CONFIG_CPU_FREQ=y', 'CONFIG_SFI=y', 'CONFIG_CPU_FREQ_GOV_PERFORMANCE=y']:
        if required not in config:
            raise ValueError('matching kernel config lacks ' + required)
    release = (kernel / 'include/config/kernel.release').read_text().strip()
    vermagic = subprocess.check_output(['modinfo', '-F', 'vermagic', str(module)], text=True).strip()
    if not vermagic or vermagic.split()[0] != release:
        raise ValueError('module vermagic differs from matching kernel build')
    kernel_id = file_build_id(kernel / 'vmlinux')
    module_id = file_build_id(module)
    payload = {'santos-sfi-cpufreq.ko': module.read_bytes()}
    for name in ['santos-cpufreq-service.py', 'service-run', 'service-log-run']:
        payload[name] = (ROOT / 'integration/cpufreq' / name).read_bytes()
    manifest = {
        'owner': 'santos-cpufreq-performance-20261002',
        'kernel_build_id': kernel_id,
        'module_build_id': module_id,
        'profile': {'governor': 'performance', 'min_khz': 800000, 'max_khz': 1600000,
                    'lease_ms': 60000, 'heartbeat_seconds': 10},
        'files': {name: hashlib.sha256(raw).hexdigest() for name, raw in payload.items()},
        'thermal_MSR_writes': False,
        'battery_or_brightness_caps': False,
        'kernel_controls_require_instance_token': True,
    }
    payload['manifest.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
    payload['disable-service.py'] = (ROOT / 'integration/cpufreq/disable-service.py').read_bytes()
    destination.mkdir(parents=True)
    for name, raw in payload.items():
        target = destination / name
        target.write_bytes(raw)
        target.chmod(0o755 if name.startswith('service-') else 0o644)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernel_build', type=Path)
    parser.add_argument('module', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    try:
        result = prepare(args.kernel_build, args.module, args.destination)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.error(str(error))
    print(json.dumps({'bundle': str(args.destination), 'kernel_build_id': result['kernel_build_id'],
                      'module_build_id': result['module_build_id'],
                      'profile': result['profile'], 'installed_or_loaded': False}, indent=2))


if __name__ == '__main__':
    main()
