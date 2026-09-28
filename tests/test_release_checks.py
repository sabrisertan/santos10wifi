#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Regression tests for editorial changes versus source-integrity checks."""
import shutil,subprocess,sys,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

class ReleaseChecks(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
  self.root=Path(self.tmp.name);(self.root/'tools').mkdir()
  for name in ('verify-release.py','update-manifest.py'):
   shutil.copyfile(ROOT/'tools'/name,self.root/'tools'/name)
  (self.root/'README.md').write_text('# Documentation\n')
  (self.root/'source.py').write_text('answer = 42\n')
  (self.root/'SYMLINKS.json').write_text('{}\n')
  self.update()
 def command(self,name):
  return subprocess.run([sys.executable,str(self.root/'tools'/name)],capture_output=True,text=True)
 def update(self):self.assertEqual(self.command('update-manifest.py').returncode,0)
 def test_web_readme_and_new_document_need_no_hash_refresh(self):
  (self.root/'README.md').write_text('# Updated\n<img src="https://example.org/screenshot.png" />\n')
  (self.root/'REPRODUCING.md').write_text('[Readme](README.md)\n')
  result=self.command('verify-release.py');self.assertEqual(result.returncode,0,result.stderr)
 def test_source_edit_still_fails_without_hash_refresh(self):
  (self.root/'source.py').write_text('answer = 43\n')
  result=self.command('verify-release.py');self.assertNotEqual(result.returncode,0)
  self.assertIn('hash mismatch: source.py',result.stderr)
 def test_documentation_links_still_checked(self):
  (self.root/'README.md').write_text('[Missing](does-not-exist.md)\n')
  self.assertIn('broken link',self.command('verify-release.py').stderr)
 def test_documentation_private_data_still_checked(self):
  marker='-----BEGIN '+'OPENSSH '+'PRIVATE KEY-----'
  (self.root/'README.md').write_text(marker+'\n')
  self.assertIn('private-data pattern',self.command('verify-release.py').stderr)
 def test_binary_payload_rejected_even_with_current_manifest(self):
  (self.root/'module.ko').write_bytes(b'\x7fELF\0')
  self.update();result=self.command('verify-release.py')
  self.assertNotEqual(result.returncode,0);self.assertIn('forbidden payload',result.stderr)

if __name__=='__main__':unittest.main()
