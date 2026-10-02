#!/usr/bin/env python3
"""Disable only the verified task-owned CPUFreq service, never graphics modules."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

BASE=Path('/opt/santos-cpufreq')
SERVICE=Path('/etc/sv/santos-cpufreq')
ALIAS=Path('/var/service/santos-cpufreq')
TOKEN=b'cpufreq-aggressive-policy-20261002: staged disabled\n'
manifest=json.loads((BASE/'manifest.json').read_text())
assert ALIAS.is_symlink() and os.readlink(ALIAS)==str(SERVICE)
assert hashlib.sha256((SERVICE/'run').read_bytes()).hexdigest()==manifest['files']['service-run']
marker=SERVICE/'down'
if marker.exists():assert marker.read_bytes()==TOKEN
else:marker.write_bytes(TOKEN)
result=subprocess.run(['sv','-w','15','down',str(ALIAS)],capture_output=True,text=True,timeout=20)
record={'command':['sv','-w','15','down',str(ALIAS)],'exit_code':result.returncode,
        'stdout':result.stdout,'stderr':result.stderr,'persistent_down_marker':True,
        'module_absent':not Path('/sys/module/santos_sfi_cpufreq').exists(),
        'policies_absent':not list(Path('/sys/devices/system/cpu/cpufreq').glob('policy*'))}
state=Path('/run/santos-cpufreq/state.json')
if state.exists():
    data=json.loads(state.read_text());assert data['owner']=='santos-cpufreq-performance-20261002'
    record['cleanup']=data.get('cleanup')
    record['controls_restored']=data.get('phase')=='stopped' and data.get('cleanup',{}).get('controls_restored') is True
else:record['controls_restored']=None
record['stop_verified']=result.returncode==0 and record['module_absent'] and record['policies_absent']
print(json.dumps(record,indent=2))
assert record['stop_verified'],'unverified stop; preserve module/evidence, no force unload'
