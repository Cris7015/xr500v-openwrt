#!/usr/bin/env python3
"""Generate a ucode fixture harness from the real backend (no hardware access).

Usage: python3 scripts/test_luci_voip_backend.py | ucode -
It can also be piped into ucode over SSH; no real fs/ubus/uci imports remain.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parent.parent
source = (root / "overlay/package/luci-app-xr500v-voip/root/usr/share/rpcd/ucode/xr500v.voip").read_text()
assert not re.search(r"\b(system|popen|writefile|exec)\s*\(", source[:source.index('const setting_fields')])
source = re.sub(r"^import .*;\n", "", source, flags=re.M)
source = source.replace("let now = time();", "let now = testTime;")
source = source.replace("return { 'xr500v.voip':", "const plugin = { 'xr500v.voip':")
prefix = r'''
let testTime = 100;
let reads = 0;
let files = {
 '/tmp/sysinfo/board_name': 'tplink,archer-xr500v-v1\n',
 '/sys/module/pcm_en751221': '', '/sys/module/econet_slic': '',
 '/sys/kernel/debug/pcm-en751221/dual_status': 'running=1 rx_frames=200 tx_frames=200 play_limit=640\nphone1 opened=0 rx_dma=0 tx_dma=0 cap=0 play=0 read_bytes=0 write_bytes=0 tx_short_frames=0 cap_drop_frames=0\nphone2 opened=1 rx_dma=1 tx_dma=1 cap=16 play=32 read_bytes=100 write_bytes=200 tx_short_frames=2 cap_drop_frames=3\n',
 '/sys/kernel/debug/econet-slic/phone1/status': 'phone=1 ec=2 ring_requested=0 hook_cache=0\nstate=0x0c\ntx_slot=0x04\nrx_slot=0x04\n',
 '/sys/kernel/debug/econet-slic/phone2/status': 'phone=2 ec=1 ring_requested=0 hook_cache=1\ntx_slot=0x06\nrx_slot=0x06\n',
 '/sys/class/leds/green:phone1/trigger': '[none] timer',
 '/sys/class/leds/green:phone2/trigger': 'none [timer]',
 '/var/run/xr500v-voip/phone1/accounts': '# comment\n<sip:secret-user@example.invalid>;auth_pass=DO_NOT_EXPOSE;regint=0\n',
 '/var/run/xr500v-voip/phone2/accounts': '<sip:secret-user@example.invalid>;auth_pass=DO_NOT_EXPOSE;regint=60\n'
};
let options = { enabled: '1', echo_cancel: '1', noise_suppress: '1', playback_percent: '50' };
let available = true;
function readfile(path) { reads++; return files[path]; }
function stat(path) { return files[path] == null ? null : { type: 'file' }; }
function cursor() { return { get: function(pkg, section, key) { return options[key]; }, unload: function() {} }; }
function connect() { return { call: function(object, method, args) {
 if (!available) return null;
 if (object == 'service') return { voip: { instances: {
  'phone1-sip': { running: true }, 'phone1-manager': { running: true },
  'phone2-sip': { running: true }, 'phone2-manager': { running: true }
 } } };
 return { up: false };
} }; }
function check(ok, reason) { if (!ok) { warn('FAIL: '+reason+'\n'); exit(1); } }
'''
suffix = r'''
let a = plugin['xr500v.voip'].status.call({ path: '/etc/shadow' });
check(a.available && a.processes_running == 4 && a.wan_up == false, 'service/WAN');
check(a.lines[0].hook == 'on_hook' && a.lines[0].ec == 2 && a.lines[0].tx_slot == 4, 'numeric and hook parser');
check(a.lines[0].capture_drops == 0 && a.lines[1].capture_drops == 3, 'real zero and nonzero');
check(a.lines[1].ringing && a.lines[1].audio_open, 'ring cadence and stream');
check(a.lines[0].account_mode == 'local' && a.lines[1].account_mode == 'registration', 'account modes');
check(a.lines[1].registration == 'not_observed', 'never invent SIP registration');
check(index(sprintf('%J',a),'DO_NOT_EXPOSE') < 0 && index(sprintf('%J',a),'secret-user') < 0, 'no credentials');
let oldreads = reads;
testTime = 104; plugin['xr500v.voip'].status.call();
check(reads == oldreads, '5 second cache');
files['/sys/kernel/debug/econet-slic/phone1/status'] = null;
files['/sys/kernel/debug/pcm-en751221/dual_status'] = null;
files['/var/run/xr500v-voip/phone1/accounts'] = null;
options.playback_percent = '999';
available = false; testTime = 110;
let b = plugin['xr500v.voip'].status.call();
check(b.lines[0].hook == 'unknown' && b.lines[0].capture_drops == null && b.lines[0].audio_open == null, 'missing data is not zero');
check(b.lines[0].sip_running == null && b.audio.playback_percent == null && b.wan_up == null, 'unknown service and invalid gain');
files['/tmp/sysinfo/board_name'] = 'another,board'; testTime = 120;
check(!plugin['xr500v.voip'].status.call().available, 'board guard');
print('PASS: backend fixtures, privacy, cache, unknown states, ring/PCM and board guard\n');
'''
print(prefix + source + suffix)
