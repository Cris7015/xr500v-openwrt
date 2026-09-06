#!/usr/bin/env python3
"""Emit ucode tests of config_apply with fully mocked files, UCI and service."""
import re
from pathlib import Path

root = Path(__file__).resolve().parent.parent
source = (root / "overlay/package/luci-app-xr500v-voip/root/usr/share/rpcd/ucode/xr500v.voip").read_text()
assert "system([ '/etc/init.d/voip', 'restart' ])" in source
assert source.count('system(') == 1
source = re.sub(r'^import .*;\n', '', source, flags=re.M)
source = source.replace("return { 'xr500v.voip':", "const plugin = { 'xr500v.voip':")
prefix = r'''
function check(value, label) { if(!value){warn('FAIL '+label+'\n');exit(1);} }
let db = { 'globals.enabled':'1','phone1.enabled':'1','phone2.enabled':'1',
 'globals.echo_cancel':'1','globals.noise_suppress':'1','globals.playback_percent':'50',
 'globals.unrelated':'DO_NOT_EXPOSE' };
let pending = {};
let writes=0,commits=0,restarts=0,backups=0;
let backupFails=false,commitFails=false,restartFails=false,serviceUnknown=false;
let files = {
 '/tmp/sysinfo/board_name':'tplink,archer-xr500v-v1',
 '/etc/config/xr500v-voip':'PRIVATE_ORIGINAL_CONFIG',
 '/sys/kernel/debug/pcm-en751221/dual_status':'running=1\nphone1 opened=0\nphone2 opened=0\n',
 '/sys/kernel/debug/econet-slic/phone1/status':'phone=1 ring_requested=0 hook_cache=0',
 '/sys/kernel/debug/econet-slic/phone2/status':'phone=2 ring_requested=0 hook_cache=0',
 '/sys/class/leds/green:phone1/trigger':'[none] timer',
 '/sys/class/leds/green:phone2/trigger':'[none] timer'
};
function readfile(path) { return files[path]; }
function stat(path) { return files[path]==null ? null : { type:'file' }; }
function lstat(path) { return { type:'directory',uid:0,mode:448 }; }
function mkdir(path, mode) { return true; }
function open(path, flags, mode) {
 if (backupFails) return null;
 check(index(path,'/root/xr500v-voip-ui-backups/config-')==0 && flags=='wxe' && mode==384,'private exclusive backup');
 backups++;
 return { write:function(raw) { check(raw=='PRIVATE_ORIGINAL_CONFIG','backup original');return length(raw); },flush:function(){return true;},close:function(){return true;} };
}
function cursor() { return {
 get:function(pkg,sec,key){return pending[sec+'.'+key] ?? db[sec+'.'+key];},
 unload:function(pkg){pending={};},
 set:function(pkg,sec,key,value){check(pkg=='xr500v-voip','only telephony UCI');pending[sec+'.'+key]=value;writes++;return true;},
 commit:function(pkg){if(commitFails)return false;for(let k,v in pending)db[k]=v;pending={};commits++;return true;}
}; }
function connect() { return { call:function(object,method,args) {
 check(object=='service' && method=='list' && args.name=='voip','guard only queries service');
 if(serviceUnknown)return null;
 return {voip:{instances:{'phone1-sip':{running:true},'phone1-manager':{running:true},'phone2-sip':{running:true},'phone2-manager':{running:true}}}};
} }; }
function system(args) { check(length(args)==2 && args[0]=='/etc/init.d/voip' && args[1]=='restart','fixed command');restarts++;return restartFails?1:0; }
'''
suffix = r'''
function request(gain) {
 let c=plugin['xr500v.voip'].config_get.call();
 let a={};for(let k,v in c.settings)a[k]=v;
 a.revision=c.revision;a.confirm=true;a.playback_percent=gain;
 return a;
}
function apply(a) { return plugin['xr500v.voip'].config_apply.call({args:a}); }
let a=request(55);a.confirm=false;
check(apply(a).error=='confirmation_required','confirmation');
a=request(101);check(apply(a).error=='invalid_volume','range');
a=request('50;command');check(apply(a).error=='invalid_volume','injection/type');
a=request(55);a.phone1_enabled='1';check(apply(a).error=='invalid_option','boolean type');
a=request(55);a.revision='stale';check(apply(a).error=='configuration_changed','optimistic revision');
check(writes==0 && restarts==0 && backups==0,'validation no writes');
files['/sys/kernel/debug/econet-slic/phone1/status']='phone=1 ring_requested=0 hook_cache=1';
let busy_result=apply(request(55));
check(busy_result.error=='line_busy','off hook: '+sprintf('%J',busy_result));
check(apply(request(50)).changed==false,'no-op even while busy');
files['/sys/kernel/debug/econet-slic/phone1/status']='phone=1 ring_requested=0 hook_cache=0';
files['/sys/class/leds/green:phone2/trigger']='none [timer]';
check(apply(request(55)).error=='line_busy','ring cadence gap');
files['/sys/class/leds/green:phone2/trigger']='[none] timer';
files['/sys/kernel/debug/pcm-en751221/dual_status']='phone1 opened=1\nphone2 opened=0';
check(apply(request(55)).error=='line_busy','audio active');
files['/sys/kernel/debug/pcm-en751221/dual_status']=null;
check(apply(request(55)).error=='cannot_verify_idle','unknown idle');
files['/sys/kernel/debug/pcm-en751221/dual_status']='phone1 opened=0\nphone2 opened=0';
serviceUnknown=true;check(apply(request(55)).error=='cannot_verify_service','service unavailable');serviceUnknown=false;
backupFails=true;check(apply(request(55)).error=='backup_failed','backup failure');backupFails=false;
check(writes==0 && restarts==0 && commits==0,'guards no writes');
let result=apply(request(55));
check(result.ok && result.saved && result.restarted && db['globals.playback_percent']=='55','save and restart');
check(writes==6 && commits==1 && restarts==1 && backups==1,'exactly one commit/restart');
check(db['globals.unrelated']=='DO_NOT_EXPOSE','unrelated settings preserved');
check(index(sprintf('%J',result),'PRIVATE_ORIGINAL_CONFIG')<0 && index(sprintf('%J',result),'DO_NOT_EXPOSE')<0,'no secrets');
check(apply(request(55)).changed==false && restarts==1,'idempotent no-op');
commitFails=true;result=apply(request(60));commitFails=false;
check(result.error=='save_failed' && restarts==1 && db['globals.playback_percent']=='55','failed commit no restart');
restartFails=true;result=apply(request(60));restartFails=false;
check(!result.ok && result.saved && result.error=='restart_failed' && restarts==2,'restart failure explicit, no automatic retry');
print('PASS: configuration validation, no-op, busy/unknown guard, private backup, whitelist, revision, failure handling\n');
'''
if __name__ == '__main__':
    print(prefix + source + suffix)
