#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise pinned checkout reconstruction with an annotated upstream tag."""
import json,shutil,subprocess,sys,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

class PrepareSource(unittest.TestCase):
 def test_peeled_tag_commit_and_existing_destination_guard(self):
  with tempfile.TemporaryDirectory() as td:
   root=Path(td);up=root/'upstream';release=root/'release';out=root/'result'
   up.mkdir();(release/'tools').mkdir(parents=True)
   shutil.copyfile(ROOT/'tools/prepare-source.py',release/'tools/prepare-source.py')
   def git(*args):
    return subprocess.check_output(['git','-C',str(up),*args],stderr=subprocess.DEVNULL,text=True).strip()
   git('init','-q');git('config','user.name','Fixture');git('config','user.email','fixture@localhost')
   (up/'example.txt').write_text('old\n');git('add','.');git('commit','-qm','Base')
   git('tag','-a','test-release','-m','Annotated tag')
   commit=git('rev-parse','HEAD');tag=git('rev-parse','test-release');self.assertNotEqual(commit,tag)
   (release/'change.patch').write_text('diff --git a/example.txt b/example.txt\n--- a/example.txt\n+++ b/example.txt\n@@ -1 +1 @@\n-old\n+new\n')
   def manifest(base):
    (release/'SOURCE-MANIFEST.json').write_text(json.dumps({'components':[{'name':'gtk','base':base,'patch':'change.patch'}]}))
   def prepare():
    return subprocess.run([sys.executable,str(release/'tools/prepare-source.py'),'gtk',str(up),str(out)],capture_output=True,text=True)
   manifest(tag);result=prepare();self.assertNotEqual(result.returncode,0);self.assertFalse(out.exists())
   manifest(commit);result=prepare();self.assertEqual(result.returncode,0,result.stderr)
   self.assertEqual((out/'example.txt').read_text(),'new\n');self.assertEqual((up/'example.txt').read_text(),'old\n')
   result=prepare();self.assertNotEqual(result.returncode,0);self.assertIn('Destination already exists',result.stderr)

if __name__=='__main__':unittest.main()
