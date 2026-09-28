#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Offline source-package integrity, private-data and link checks (no device I/O)."""
import hashlib,json,re,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parents[1]
def included(p):
 parts=p.relative_to(root).parts
 return '.git' not in parts and '__pycache__' not in parts and parts[0] not in ('work','out','dist')
errors=[]
files={p.relative_to(root).as_posix():p for p in root.rglob('*')
       if included(p) and p.is_file() and not p.is_symlink()}
expected={}
for line in (root/'SHA256SUMS').read_text().splitlines():
 digest,name=line.split('  ',1)
 if name in expected:errors.append(f'duplicate manifest entry: {name}')
 expected[name]=digest
# Markdown remains subject to hygiene/link checks, but normal editorial edits
# do not invalidate the source/build-input checksum manifest.
actual={name for name in files if name!='SHA256SUMS' and not name.lower().endswith('.md')}
if actual!=set(expected):
 errors.append(f'manifest inventory mismatch: missing={sorted(set(expected)-actual)}, extra={sorted(actual-set(expected))}')
secret_patterns=[rb'-----BEGIN (?:OPENSSH |RSA |EC |DSA )?PRIVATE KEY-----',
                 rb'gh[pousr]_[A-Za-z0-9]{30,}',rb'github_pat_[A-Za-z0-9_]{40,}',
                 rb'AKIA[A-Z0-9]{16}',rb'/home/(?!santos(?:/|\b))[A-Za-z][A-Za-z0-9_-]*/',rb'10\.42\.0\.\d+']
for name,p in files.items():
 data=p.read_bytes()
 if name in expected and hashlib.sha256(data).hexdigest()!=expected[name]:errors.append(f'hash mismatch: {name} (after reviewing source changes, run python3 tools/update-manifest.py)')
 if b'\0' in data:errors.append(f'binary content: {name}')
 if p.stat().st_size>10*1024*1024:errors.append(f'oversized source file: {name}')
 if any(re.search(pattern,data) for pattern in secret_patterns):errors.append(f'private-data pattern: {name}')
 if p.name in ('id_rsa','id_ed25519','authorized_keys','known_hosts','.env') or p.suffix in ('.ko','.o','.img','.xbps','.pem','.key'):
  errors.append(f'forbidden payload: {name}')
 if p.suffix=='.py':
  try:compile(data,str(p),'exec')
  except SyntaxError as e:errors.append(f'python syntax: {name}: {e}')
 if data.startswith(b'#!/bin/sh'):
  check=subprocess.run(['sh','-n',str(p)],capture_output=True,text=True)
  if check.returncode:errors.append(f'shell syntax: {name}: {check.stderr}')
 if p.suffix=='.md':
  for target in re.findall(r'\[[^\]]*\]\(([^)]+)\)',data.decode()):
   if '://' in target or target.startswith('#'):continue
   if not (p.parent/target.split('#',1)[0]).exists():errors.append(f'broken link: {name}: {target}')
links=json.loads((root/'SYMLINKS.json').read_text())
found={p.relative_to(root).as_posix():p.readlink().as_posix() for p in root.rglob('*') if included(p) and p.is_symlink()}
if links!=found:errors.append('symlink inventory mismatch')
for name,target in found.items():
 p=root/name
 if Path(target).is_absolute() or not p.resolve().is_relative_to(root) or not p.exists():errors.append(f'external or broken symlink: {name}')
if errors:
 print('\n'.join(errors),file=sys.stderr);sys.exit(1)
print(f'PASS: {len(expected)} hashed files, {len(links)} internal symlinks; source checksums, syntax, hygiene and documentation links checked.')
print('This is a bounded pattern scan, not a comprehensive secret audit or hardware test.')
