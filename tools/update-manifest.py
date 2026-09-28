#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Regenerate hashes after reviewing intentional changes. Does not stage files."""
import hashlib,json
from pathlib import Path
r=Path(__file__).resolve().parents[1]
paths=[p for p in r.rglob('*') if '.git' not in p.relative_to(r).parts
       and p.relative_to(r).parts[0] not in ('work','out','dist')
       and '__pycache__' not in p.relative_to(r).parts]
links={p.relative_to(r).as_posix():p.readlink().as_posix() for p in paths if p.is_symlink()}
(r/'SYMLINKS.json').write_text(json.dumps(links,indent=2,sort_keys=True)+'\n')
files=sorted(p for p in paths if p.is_file() and not p.is_symlink() and p.name!='SHA256SUMS')
(r/'SHA256SUMS').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+p.relative_to(r).as_posix()+'\n' for p in files))
print(f'Wrote {len(files)} file hashes and {len(links)} symlinks')
