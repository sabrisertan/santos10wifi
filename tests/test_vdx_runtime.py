#!/usr/bin/env python3
"""Offline persistent-bootstrap and future-only rollback lifecycle tests."""
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('santos_vdx_runtime_test', ROOT / 'integration/vdx/santos-vdx-runtime.py')
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)
BOOT = 'ab7274c4-196b-464d-aca4-084d84abf2d5'
OTHER = '12345678-1111-2222-3333-444444444444'


class BootPlans(unittest.TestCase):
    expected = {'shell': 'r7-shell', 'engine': 'r7-engine'}

    def test_fresh_boot_starts_once(self):
        self.assertEqual(runtime.plan_boot(None, BOOT, {}, self.expected, False, False), 'bootstrap')
        previous = {'owner': runtime.OWNER, 'boot_id': BOOT, 'profile': 'r7'}
        self.assertEqual(runtime.plan_boot(previous, BOOT, {}, self.expected, False, False), 'hold')

    def test_ready_retains_existing_pair(self):
        self.assertEqual(runtime.plan_boot(None, BOOT, self.expected, self.expected, True, True), 'hold')

    def test_failed_or_incomplete_attempt_is_not_retried(self):
        self.assertEqual(runtime.plan_boot(None, BOOT, {'shell': 'r7-shell'}, self.expected, False, True), 'hold')
        with self.assertRaises(runtime.RuntimeErrorState):
            runtime.plan_boot(None, BOOT, {'shell': 'r7-shell'}, self.expected, False, False)

    def test_ready_with_partial_pair_is_refused(self):
        with self.assertRaises(runtime.RuntimeErrorState):
            runtime.plan_boot(None, BOOT, {'shell': 'r7-shell'}, self.expected, True, True)

    def test_wrong_or_mixed_loaded_code_is_refused(self):
        for actual in [{'shell': 'old'}, {'shell': 'r7-shell', 'engine': 'old'}, {'unknown': 'r7'}]:
            with self.subTest(actual=actual), self.assertRaises(runtime.RuntimeErrorState):
                runtime.plan_boot(None, BOOT, actual, self.expected, True, True)

    def test_other_owner_boot_or_profile_receipt_is_refused(self):
        for key, bad in [('owner', 'user'), ('boot_id', OTHER), ('profile', 'baseline')]:
            prior = {'owner': runtime.OWNER, 'boot_id': BOOT, 'profile': 'r7'}
            prior[key] = bad
            with self.subTest(key=key), self.assertRaises(runtime.RuntimeErrorState):
                runtime.plan_boot(prior, BOOT, {}, self.expected, False, False)


class OwnedFiles(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.base = root / 'bundle'
        self.base.mkdir()
        (self.base / 'baseline').mkdir()
        (self.base / 'modules').mkdir()
        self.old = b'#!/bin/sh\n# retained baseline\n'
        self.new = b'#!/bin/sh\n# permanent runtime launcher\n'
        (self.base / 'baseline/run.highmem-p11').write_bytes(self.old)
        (self.base / 'run.modules').write_bytes(b'#!/bin/sh\n# tested module bootstrap\n')
        (self.base / 'modules/shell.ko').write_bytes(b'r7-shell-binary')
        self.live = root / 'live-loader'
        self.live.write_bytes(self.old)
        self.ready = root / 'ready'
        self.ready.touch()
        self.attempted = root / 'attempted'
        self.manifest = {'owner': runtime.OWNER, 'format': 1,
                         'files': {'run.modules': runtime.digest(self.base / 'run.modules'),
                                   'modules/shell.ko': runtime.digest(self.base / 'modules/shell.ko')},
                         'baseline_loader_sha256': hashlib.sha256(self.old).hexdigest(),
                         'launcher_sha256': hashlib.sha256(self.new).hexdigest(),
                         'launcher': self.new.decode(), 'loader_mode': 0o755,
                         'promotion_boot': BOOT, 'loaded_ids': {'shell': 'r7', 'engine': 'r7'},
                         'promotion_protected_files': {}, 'original_directory_files': {}}
        self.selection = {'owner': runtime.OWNER, 'format': 1, 'selected': 'staged'}
        patcher = mock.patch.multiple(runtime, BASE=self.base, LOADER=self.live, READY=self.ready,
                                     ATTEMPTED=self.attempted, RUNTIME=root / 'run')
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_promote_changes_only_future_loader(self):
        result = runtime.promote(self.manifest, self.selection, BOOT, self.manifest['loaded_ids'])
        self.assertEqual(self.live.read_bytes(), self.new)
        self.assertFalse(result['current_modules_reloaded_or_rebooted'])
        self.assertFalse(result['persistent_coldboot_retested'])
        self.assertEqual(json.loads((self.base / 'selection.json').read_text())['selected'], 'r7')
        self.assertEqual((self.base / 'baseline/run.highmem-p11').read_bytes(), self.old)

    def test_promotion_refuses_wrong_boot_or_incomplete_live_pair(self):
        for boot, ids in [(OTHER, self.manifest['loaded_ids']), (BOOT, {'shell': 'r7'}),
                          (BOOT, {'shell': 'old', 'engine': 'r7'})]:
            with self.subTest(boot=boot, ids=ids), self.assertRaises(runtime.RuntimeErrorState):
                runtime.promote(self.manifest, self.selection, boot, ids)
        self.assertEqual(self.live.read_bytes(), self.old)

    def test_tampered_module_or_bootstrap_refuses_promotion(self):
        for name in ['modules/shell.ko', 'run.modules']:
            with self.subTest(name=name):
                path = self.base / name
                raw = path.read_bytes()
                path.write_bytes(b'changed')
                with self.assertRaises(runtime.RuntimeErrorState):
                    runtime.promote(self.manifest, self.selection, BOOT, self.manifest['loaded_ids'])
                path.write_bytes(raw)
        self.assertEqual(self.live.read_bytes(), self.old)

    def test_unknown_user_loader_is_preserved(self):
        self.live.write_bytes(b'user-change')
        with self.assertRaises(runtime.RuntimeErrorState):
            runtime.promote(self.manifest, self.selection, BOOT, self.manifest['loaded_ids'])
        with self.assertRaises(runtime.RuntimeErrorState):
            runtime.rollback(self.manifest, self.selection, BOOT, {})
        self.assertEqual(self.live.read_bytes(), b'user-change')

    def test_rollback_is_future_only_and_idempotent(self):
        self.live.write_bytes(self.new)
        selection = dict(self.selection, selected='r7')
        ids = self.manifest['loaded_ids']
        first = runtime.rollback(self.manifest, selection, BOOT, ids)
        second = runtime.rollback(self.manifest, dict(selection, selected='baseline'), BOOT, ids)
        self.assertEqual(first['current_loaded_ids_unchanged'], ids)
        self.assertFalse(second['current_modules_reloaded_unloaded_or_rebooted'])
        self.assertEqual(self.live.read_bytes(), self.old)

    def test_rollback_works_without_new_module_hash_health(self):
        self.live.write_bytes(self.new)
        (self.base / 'modules/shell.ko').write_bytes(b'broken')
        result = runtime.rollback(self.manifest, dict(self.selection, selected='r7'), OTHER, {})
        self.assertEqual(result['selected'], 'baseline-for-future-boots')
        self.assertEqual(self.live.read_bytes(), self.old)

    def test_rollback_refuses_wrong_owner_or_bad_backup(self):
        self.live.write_bytes(self.new)
        manifest = dict(self.manifest, owner='other')
        with self.assertRaises(runtime.RuntimeErrorState):
            runtime.rollback(manifest, self.selection, BOOT, {})
        (self.base / 'baseline/run.highmem-p11').write_bytes(b'bad')
        with self.assertRaises(runtime.RuntimeErrorState):
            runtime.rollback(self.manifest, self.selection, BOOT, {})
        self.assertEqual(self.live.read_bytes(), self.new)

    def test_path_escape_and_symlink_files_refused(self):
        manifest = copy.deepcopy(self.manifest)
        manifest['files']['../outside'] = 'bad'
        with self.assertRaises(runtime.RuntimeErrorState):
            runtime.verify_bundle(manifest)
        path = self.base / 'modules/shell.ko'
        path.unlink()
        path.symlink_to(self.live)
        with self.assertRaises(runtime.RuntimeErrorState):
            runtime.verify_bundle(self.manifest)

    def test_symlink_receipt_cannot_overwrite_user_file(self):
        path = self.base / 'selection.json'
        path.symlink_to(self.live)
        with self.assertRaises(runtime.RuntimeErrorState):
            runtime.write_json(path, self.selection)
        self.assertEqual(self.live.read_bytes(), self.old)

    def test_invalid_selection_is_refused(self):
        for key, value in [('owner', 'other'), ('format', 2), ('selected', 'unknown')]:
            selection = dict(self.selection, **{key: value})
            with self.subTest(key=key), self.assertRaises(runtime.RuntimeErrorState):
                runtime.validate_selection(selection)

    def test_durable_claim_prevents_second_bootstrap(self):
        self.ready.unlink()
        with mock.patch.object(runtime, 'owned_runtime_directory', return_value=True):
            self.assertEqual(runtime.claim_boot(BOOT, {}, self.manifest), 'bootstrap')
            path = runtime.RUNTIME / 'boot.json'
            raw = path.read_bytes()
            self.assertEqual(runtime.claim_boot(BOOT, {}, self.manifest), 'hold')
            self.assertEqual(path.read_bytes(), raw)
            with self.assertRaises(runtime.RuntimeErrorState):
                runtime.claim_boot(OTHER, {}, self.manifest)
        self.assertEqual(path.read_bytes(), raw)

    def test_unknown_runtime_directory_is_not_adopted(self):
        runtime.RUNTIME.mkdir()
        sentinel = runtime.RUNTIME / 'sentinel'
        sentinel.write_text('preserve')
        with self.assertRaises(runtime.RuntimeErrorState):
            runtime.claim_boot(BOOT, {}, self.manifest)
        self.assertEqual(sentinel.read_text(), 'preserve')

    def test_main_verify_has_no_bootstrap_claim_or_exec(self):
        self.live.write_bytes(self.new)
        runtime.write_json(self.base / 'manifest.json', dict(self.manifest, kernel_build_id='abc'))
        runtime.write_json(self.base / 'selection.json', dict(self.selection, selected='r7'))
        with mock.patch.object(runtime.os, 'geteuid', return_value=0), \
                mock.patch.object(runtime, 'owned_deployment_root', return_value=True), \
                mock.patch.object(runtime, 'boot_id', return_value=BOOT), \
                mock.patch.object(runtime, 'loaded_ids', return_value=self.manifest['loaded_ids']), \
                mock.patch.object(runtime, 'kernel_ids', return_value=['abc']), \
                mock.patch.object(runtime, 'claim_boot') as claim, \
                mock.patch.object(runtime.os, 'execv') as execv, \
                mock.patch.object(runtime.sys, 'argv', ['runtime', '--verify']), \
                mock.patch.object(runtime.sys, 'stdout', new_callable=io.StringIO) as output:
            runtime.main()
        self.assertFalse(json.loads(output.getvalue())['bootstrap_or_hardware_action'])
        claim.assert_not_called()
        execv.assert_not_called()
        self.assertFalse(runtime.RUNTIME.exists())

    def test_main_refuses_wrong_kernel_before_bootstrap(self):
        self.live.write_bytes(self.new)
        runtime.write_json(self.base / 'manifest.json', dict(self.manifest, kernel_build_id='abc'))
        runtime.write_json(self.base / 'selection.json', dict(self.selection, selected='r7'))
        with mock.patch.object(runtime.os, 'geteuid', return_value=0), \
                mock.patch.object(runtime, 'owned_deployment_root', return_value=True), \
                mock.patch.object(runtime, 'boot_id', return_value=BOOT), \
                mock.patch.object(runtime, 'loaded_ids', return_value={}), \
                mock.patch.object(runtime, 'kernel_ids', return_value=['other']), \
                mock.patch.object(runtime, 'claim_boot') as claim, \
                mock.patch.object(runtime.os, 'execv') as execv, \
                mock.patch.object(runtime.sys, 'argv', ['runtime', '--run']):
            with self.assertRaises(runtime.RuntimeErrorState):
                runtime.main()
        claim.assert_not_called()
        execv.assert_not_called()

    def test_main_fresh_boot_executes_only_verified_bootstrap(self):
        self.ready.unlink()
        self.live.write_bytes(self.new)
        runtime.write_json(self.base / 'manifest.json', dict(self.manifest, kernel_build_id='abc'))
        runtime.write_json(self.base / 'selection.json', dict(self.selection, selected='r7'))
        with mock.patch.object(runtime.os, 'geteuid', return_value=0), \
                mock.patch.object(runtime, 'owned_deployment_root', return_value=True), \
                mock.patch.object(runtime, 'boot_id', return_value=BOOT), \
                mock.patch.object(runtime, 'loaded_ids', return_value={}), \
                mock.patch.object(runtime, 'kernel_ids', return_value=['abc']), \
                mock.patch.object(runtime.os, 'execv', side_effect=SystemExit(99)) as execv, \
                mock.patch.object(runtime.sys, 'argv', ['runtime', '--run']), \
                mock.patch.object(runtime.sys, 'stdout', new_callable=io.StringIO):
            with self.assertRaisesRegex(SystemExit, '99'):
                runtime.main()
        execv.assert_called_once_with('/bin/sh', ['/bin/sh', str(self.base / 'run.modules')])
        self.assertTrue(json.loads((runtime.RUNTIME / 'boot.json').read_text())['bootstrap_claimed'])


if __name__ == '__main__':
    unittest.main()
