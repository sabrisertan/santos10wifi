#!/usr/bin/python3
"""Tablet integration check: ordinary mpv, silent playback, pause/EOF leases."""
import json
import os
import socket
import subprocess
import time
from pathlib import Path

ipc='/run/santos-power/test-mpv.sock'
Path(ipc).unlink(missing_ok=True)

def status():
    return json.loads(subprocess.check_output(['/usr/local/bin/santos-powerctl']))['result']

def leases():
    return [x for x in status()['inhibitors'] if x['reason']=='mpv playback']

def wait_for(check, timeout=12):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        if check(): return
        time.sleep(.2)
    raise AssertionError('condition timed out')

log=open('/root/santos/power/mpv-silent.log','w')
player=subprocess.Popen(['/usr/bin/mpv', '--no-audio', '--input-ipc-server='+ipc,
                         '/root/santos/mpv-vdx/santos-perf-1080p.mp4'],stdout=log,stderr=log)
try:
    wait_for(lambda: Path(ipc).exists())
    wait_for(lambda: bool(leases()))
    result={'playing':status()}
    with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as control:
        control.connect(ipc)
        control.sendall(b'{"command":["set_property","pause",true]}\n')
        wait_for(lambda: not leases())
        result['paused']=status()
        control.sendall(b'{"command":["set_property","pause",false]}\n')
        wait_for(lambda: bool(leases()))
        result['resumed']=status()
    result['exit_code']=player.wait(timeout=20)
    wait_for(lambda: not leases())
    result['after_eof']=status()
    assert result['exit_code']==0
    Path('/root/santos/power/mpv-inhibitor-results.json').write_text(json.dumps(result,indent=2)+'\n')
    print('PASS: silent mpv inhibits; pause releases; resume renews; EOF releases, rc=0')
finally:
    if player.poll() is None:
        player.terminate()
        player.wait(timeout=8)
