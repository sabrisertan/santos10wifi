#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Offline CPUFreq core, service ownership and new-bundle regression tests."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class CpuFreqCore(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
    def test_real_portable_control_and_domain_cases(self):
        with tempfile.TemporaryDirectory() as directory:
            for source, marker in [('test_core.c', 'CORE_CHECKS=105 PASS'),
                                   ('test_domain.c', 'DOMAIN_CHECKS=221 PASS')]:
                with self.subTest(source=source):
                    binary = Path(directory) / source.removesuffix('.c')
                    result = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                                             '-I' + str(ROOT / 'modules/santos-cpufreq'),
                                             str(ROOT / 'tests/cpufreq' / source), '-o', str(binary)],
                                            capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    result = subprocess.run([str(binary)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn(marker, result.stdout)


class CpuFreqService(unittest.TestCase):
    def setUp(self):
        self.service = load('cpufreq_service_test', ROOT / 'integration/cpufreq/santos-cpufreq-service.py')
        self.data = {'domain': 'package', 'registered': True, 'expired': False,
                     'released': False, 'restored': False, 'last_error': 0, 'restore_error': 0,
                     'cpus': [{'ctl': '0x0' if cpu == 0 else '0x645',
                               'original_ctl': '0x0' if cpu == 0 else '0x645',
                               'misc': '0x123', 'initial_misc': '0x123',
                               'thermal': '0x882e0000', 'errors': [0] * 6} for cpu in range(4)]}

    def test_expired_failed_or_throttled_driver_is_not_renewed(self):
        self.service.validate_snapshot(self.data, True)
        for field, value in [('registered', False), ('expired', True), ('released', True),
                             ('domain', 'other'), ('last_error', -5), ('restore_error', -5)]:
            with self.subTest(field=field):
                data = copy.deepcopy(self.data)
                data[field] = value
                with self.assertRaises(RuntimeError):
                    self.service.validate_snapshot(data, True)
        for thermal in ['0x0', '0x882e0001', '0x882e0400']:
            with self.subTest(thermal=thermal):
                data = copy.deepcopy(self.data)
                data['cpus'][0]['thermal'] = thermal
                with self.assertRaises(RuntimeError):
                    self.service.validate_snapshot(data, True)

    def test_restoration_requires_controls_and_misc_to_match(self):
        data = copy.deepcopy(self.data)
        data.update(registered=False, released=True, restored=True)
        self.assertTrue(self.service.restored_snapshot(data))
        data['cpus'][2]['ctl'] = '0xc57'
        self.assertFalse(self.service.restored_snapshot(data))

    def test_same_binary_wrong_instance_cannot_release(self):
        with tempfile.TemporaryDirectory() as directory:
            sys = Path(directory) / 'module'
            (sys / 'parameters').mkdir(parents=True)
            (sys / 'parameters/owner_token').write_text('aaa-bbb\n')
            (sys / 'parameters/release').write_text('')
            self.service.SYS = sys
            receipt = {'owner': self.service.OWNER, 'boot_id': 'fixture-boot',
                       'module_build_id': 'fixture-build', 'instance': 'other-instance', 'phase': 'running'}
            with mock.patch.object(self.service, 'boot_id', return_value='fixture-boot'):
                with self.assertRaisesRegex(RuntimeError, 'not owned'):
                    self.service.release_owned({'module_build_id': 'fixture-build'}, receipt, 'test')
            self.assertEqual((sys / 'parameters/release').read_text(), '')

    def test_unowned_runtime_directory_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            self.service.STATE_DIR = Path(directory) / 'existing'
            self.service.STATE_DIR.mkdir()
            sentinel = self.service.STATE_DIR / 'sentinel'
            sentinel.write_text('preserve')
            with self.assertRaisesRegex(RuntimeError, 'not owned'):
                self.service.ensure_state_directory()
            self.assertEqual(sentinel.read_text(), 'preserve')

    def test_cpu0_must_be_in_package_policy(self):
        with tempfile.TemporaryDirectory() as directory:
            policy = Path(directory)
            for name, value in {'scaling_driver': 'santos-sfi', 'scaling_governor': 'performance',
                                'scaling_min_freq': '800000', 'scaling_max_freq': '1600000',
                                'related_cpus': '1 2 3'}.items():
                (policy / name).write_text(value)
            with self.assertRaisesRegex(RuntimeError, 'coverage'):
                self.service.validate_policy(policy, {'governor': 'performance', 'min_khz': 800000,
                                                       'max_khz': 1600000})

    def test_build_id_note_parser(self):
        raw = struct.pack('<III', 4, 4, 3) + b'GNU\0' + b'abcd'
        self.assertEqual(self.service.build_ids(raw), ['61626364'])


class CpuFreqBundle(unittest.TestCase):
    def setUp(self):
        self.helper = load('cpufreq_bundle_test', ROOT / 'tools/prepare-cpufreq-service.py')
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.kernel = self.root / 'kernel'
        (self.kernel / 'include/config').mkdir(parents=True)
        (self.kernel / 'include/config/kernel.release').write_text('fixture-preview\n')
        (self.kernel / '.config').write_text('CONFIG_MODULES=y\nCONFIG_CPU_FREQ=y\nCONFIG_SFI=y\nCONFIG_CPU_FREQ_GOV_PERFORMANCE=y\n')
        (self.kernel / 'vmlinux').write_bytes(b'fixture kernel')
        self.module = self.root / 'santos-sfi-cpufreq.ko'
        self.module.write_bytes(b'fixture module')

    def test_new_bundle_pins_actual_builds_and_payload(self):
        output = self.root / 'bundle'
        with mock.patch.object(self.helper.subprocess, 'check_output', return_value='fixture-preview SMP\n'), \
                mock.patch.object(self.helper, 'file_build_id', side_effect=['kernel-id', 'module-id']):
            manifest = self.helper.prepare(self.kernel, self.module, output)
        self.assertEqual(manifest['kernel_build_id'], 'kernel-id')
        self.assertEqual(manifest['module_build_id'], 'module-id')
        self.assertEqual(manifest['profile']['governor'], 'performance')
        for name, digest in manifest['files'].items():
            self.assertEqual(hashlib.sha256((output / name).read_bytes()).hexdigest(), digest)
        self.assertTrue((output / 'disable-service.py').exists())
        self.assertEqual(json.loads((output / 'manifest.json').read_text()), manifest)
        with self.assertRaisesRegex(ValueError, 'already exists'):
            self.helper.prepare(self.kernel, self.module, output)

    def test_wrong_kernel_release_never_creates_bundle(self):
        output = self.root / 'bad-bundle'
        with mock.patch.object(self.helper.subprocess, 'check_output', return_value='different-release SMP\n'):
            with self.assertRaisesRegex(ValueError, 'vermagic'):
                self.helper.prepare(self.kernel, self.module, output)
        self.assertFalse(output.exists())

    def test_missing_governor_is_not_silently_enabled(self):
        (self.kernel / '.config').write_text('CONFIG_MODULES=y\nCONFIG_CPU_FREQ=y\nCONFIG_SFI=y\n')
        with self.assertRaisesRegex(ValueError, 'PERFORMANCE'):
            self.helper.prepare(self.kernel, self.module, self.root / 'bad-bundle')

    def test_existing_module_output_is_not_overwritten(self):
        output = self.root / 'existing'
        output.mkdir()
        sentinel = output / 'sentinel'
        sentinel.write_text('preserve')
        (self.kernel / 'Module.symvers').write_text('fixture\n')
        result = subprocess.run(['sh', str(ROOT / 'tools/build-cpufreq.sh'), str(self.kernel), str(output)],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('must not exist', result.stderr)
        self.assertEqual(sentinel.read_text(), 'preserve')

    def test_full_build_helper_includes_cpufreq_without_device_action(self):
        release = self.root / 'release'
        (release / 'tools').mkdir(parents=True)
        shutil.copyfile(ROOT / 'tools/build-kernel.sh', release / 'tools/build-kernel.sh')
        (release / 'kernel/configs').mkdir(parents=True)
        (release / 'kernel/configs/santos10wifi-highmem.config').write_text('fixture config\n')
        (release / 'kernel/overlay/drivers/gpu/drm/pvrsgx/santos-112').mkdir(parents=True)
        for name in ['santos-sync-core', 'santos-vdx']:
            (release / 'modules' / name).mkdir(parents=True)
        shutil.copytree(ROOT / 'modules/santos-cpufreq', release / 'modules/santos-cpufreq')
        binaries = self.root / 'bin'
        binaries.mkdir()
        trace = self.root / 'make-trace.jsonl'
        fake_make = binaries / 'make'
        fake_make.write_text('''#!/usr/bin/env python3
import json,os,sys
from pathlib import Path
with Path(os.environ['MAKE_TRACE']).open('a') as stream:
 stream.write(json.dumps(sys.argv[1:])+'\\n')
for arg in sys.argv[1:]:
 if arg.startswith('M='):
  path=Path(arg[2:]);path.mkdir(parents=True,exist_ok=True)
  (path/'Module.symvers').write_text('0x0 santos_pvr_irq_start fixture EXPORT_SYMBOL\\n0x0 santos_pvr_irq_stop fixture EXPORT_SYMBOL\\n')
''')
        fake_make.chmod(0o755)
        environment = dict(os.environ, PATH=str(binaries) + os.pathsep + os.environ['PATH'],
                           MAKE_TRACE=str(trace))
        output = self.root / 'candidate'
        result = subprocess.run(['sh', str(release / 'tools/build-kernel.sh'),
                                 str(self.kernel), str(output)], env=environment,
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = [json.loads(line) for line in trace.read_text().splitlines()]
        self.assertTrue(any('M=' + str(output / 'cpufreq') in args and args[-1] == 'modules'
                            for args in commands))
        self.assertEqual((output / 'cpufreq/santos-sfi-cpufreq.c').read_bytes(),
                         (ROOT / 'modules/santos-cpufreq/santos-sfi-cpufreq.c').read_bytes())


if __name__ == '__main__':
    unittest.main()
