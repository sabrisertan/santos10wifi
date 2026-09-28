#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Materialize a component in a NEW directory from a pinned upstream input."""
import argparse,hashlib,json,shutil,subprocess,tarfile,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('component',choices=['linux','gtk','mutter','gnome-shell','pipeline'])
p.add_argument('upstream',type=Path,help='Pinned git checkout (linux/gtk), or pinned source tarball')
p.add_argument('destination',type=Path,help='Must not exist')
a=p.parse_args();src=a.upstream.resolve();dst=a.destination.resolve()
if dst.exists():p.error('Destination already exists; use a new directory')
if src.is_dir() and (dst==src or src in dst.parents):p.error('Destination must be outside upstream')
c=next(c for c in json.loads((root/'SOURCE-MANIFEST.json').read_text())['components'] if c['name']==a.component)
with tempfile.TemporaryDirectory(prefix='santos-upstream-') as td:
 if src.is_file():
  expected=c.get('upstream_sha256')
  if not expected or hashlib.sha256(src.read_bytes()).hexdigest()!=expected:p.error('Archive SHA256 mismatch or no pinned archive for component')
  with tarfile.open(src) as t:t.extractall(td,filter='data')
  entries=list(Path(td).iterdir())
  if len(entries)!=1 or not entries[0].is_dir():p.error('Expected a single upstream root')
  source=entries[0]
 else:
  if a.component not in ('linux','gtk'):p.error('Use the hash-pinned archive for this component')
  head=subprocess.check_output(['git','-C',str(src),'rev-parse','HEAD'],text=True).strip()
  if head!=c['base']:p.error('Upstream HEAD differs from pinned base')
  if subprocess.check_output(['git','-C',str(src),'status','--porcelain']):p.error('Upstream must be clean')
  source=src
 shutil.copytree(source,dst,symlinks=True,ignore=shutil.ignore_patterns('.git'))
 subprocess.run(['git','apply','--check',str(root/c['patch'])],cwd=dst,check=True)
 subprocess.run(['git','apply',str(root/c['patch'])],cwd=dst,check=True)
 if a.component=='linux':shutil.copytree(root/'kernel/overlay',dst,dirs_exist_ok=True)
print(dst)
