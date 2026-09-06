#!/usr/bin/env python3
"""Emit isolated ucode account RPC/renderer tests; no real config/hardware IO."""
from pathlib import Path
import re
import test_luci_voip_config as fixture

shared = (fixture.root / 'overlay/package/xr500v-voip/files/accounts.uc').read_text()
shared = re.sub(r'^export ', '', shared, flags=re.M)
prefix = fixture.prefix.replace(
    "check(object=='service' && method=='list' && args.name=='voip','guard only queries service');",
    "if(object=='network.interface' && method=='dump') return {interface:[{interface:'lan',up:true,l3_device:'br-lan','ipv4-address':[{address:'192.0.2.1'}]},{interface:'voice',up:false}]};\ncheck(object=='service' && method=='list','bounded status queries');")
prefix += "\nfunction chmod(path,mode){check(path=='/etc/config/xr500v-voip' && mode==384,'private config');return true;}\n"
suffix = r'''
function get() { return plugin['xr500v.voip'].accounts_get.call(); }
function req() {
 let c=get(), v={}; for(let k,x in c.lines[0].values)v[k]=x;
 v.password='';v.clear_password=false;
 return {line:1,revision:c.revision,confirm:true,values:v};
}
function apply(a) { return plugin['xr500v.voip'].accounts_apply.call({args:a}); }
check(get().lines[0].values.account_mode=='local' && length(get().networks)==2,'local defaults/networks');
check(apply(req()).changed==false && writes==0,'legacy no-op');
let a=req();a.values.account_mode='provider';
check(apply(a).error=='provider_required','provider validation');
a=req();a.values.dial_mode='keypad';a.values.network='lan;reboot';
check(apply(a).error=='invalid_account','network injection');
a=req();a.values.dial_mode='keypad';a.values.network='missing';
check(apply(a).error=='network_missing','missing interface');
a=req();a.values.password='secret;regint=0';
check(apply(a).error=='unsupported_credentials','password delimiter');
a=req();a.values.domain='example.org\nmodule shell.so';
check(apply(a).error=='invalid_sip_address','config injection');
a=req();a.values.auth_user='"quoted"';
check(apply(a).error=='unsupported_credentials','libre no quoting');
for(let value in ['secret\nnext','secret\tvalue','secret\rvalue','secret\x00']) {
 a=req();a.values.password=value;check(apply(a).error=='unsupported_credentials','credential control byte');
}
a=req();a.values.hotline='sip:alice@bad/host';
check(apply(a).error=='invalid_hotline','URI validation');
a=req();a.values.account_mode='custom';
check(apply(a).error=='custom_missing','custom existence');
a=req();a.values.sip_password='oops';check(apply(a).error=='invalid_account','unknown fields');
check(commits==0 && writes==0 && restarts==0,'validation never writes');
a=req();a.values.dial_mode='keypad';
files['/sys/class/leds/green:phone1/trigger']='none [timer]';
check(apply(a).error=='line_busy','ring guard');
files['/sys/class/leds/green:phone1/trigger']='[none] timer';
backupFails=true;check(apply(a).error=='backup_failed','backup guard');backupFails=false;
a=req();a.values.account_mode='provider';a.values.username='12345';a.values.domain='sip.example.invalid:5060';
a.values.proxy='192.0.2.1:5060';a.values.password='SECRET_@:$%token';a.values.network='voice';a.values.dial_mode='keypad';
let saved=apply(a);
check(saved.ok && saved.saved && saved.restarted && saved.config.lines[0].password_set,'save on configured down network');
check(index(sprintf('%J',saved),'SECRET_')<0 && index(sprintf('%J',get()),'SECRET_')<0,'no secret readback/revision');
check(db['phone1.password']=='SECRET_@:$%token' && db['globals.playback_percent']=='50','exact password/audio preserved');
check(apply(req()).changed==false && restarts==1,'provider no-op');
let before=get().revision;db['phone1.password']='REPLACED_SECRET';
check(get().revision!=before,'secret change invalidates revision without exposure');
a=req();a.values.reg_interval='600';check(apply(a).ok && db['phone1.password']=='REPLACED_SECRET','blank retains password');
a=req();a.values.clear_password=true;check(apply(a).ok && !get().lines[0].password_set,'explicit clear');
a=req();a.revision='old';check(apply(a).error=='configuration_changed','stale revision');
let p=profile(1);p.password='dummy';
let text=account_render(p,1,'192.0.2.1');
check(index(text,'<sip:12345@sip.example.invalid:5060;transport=udp>')==0,'provider AoR');
check(index(text,';auth_pass=dummy')>=0 && index(text,';outbound="sip:192.0.2.1:5060;transport=udp"')>=0,'bare auth and quoted outbound');
p.account_mode='local';
text=account_render(p,2,'192.0.2.1');
check(index(text,'<sip:phone2@192.0.2.1:5062>')==0 && index(text,'regint=0')>=0 && index(text,'auth_pass')<0,'local never registers/leaks provider credentials');
for(let host in ['example.org','10.0.0.1:5060','sip.example.org:65535'])check(sip_host(host),'valid host');
for(let host in ['1.2.3.999','1.2.3','-bad.org','good.org:0','good.org:65536','example.org;evil','foo/bar','host\r\n'])check(!sip_host(host),'invalid host');
files['/etc/xr500v-voip/phone2/accounts']='<sip:legacy@local>;auth_pass=HIDDEN';
check(get().lines[1].values.account_mode=='custom' && index(sprintf('%J',get()),'HIDDEN')<0,'legacy custom preserved/private');
print('PASS: accounts defaults, validation, injection, down/missing network, busy/backup guards, no-op, revisions, write-only secrets, renderer and legacy preservation\n');
'''
print(prefix + shared + fixture.source + suffix)
