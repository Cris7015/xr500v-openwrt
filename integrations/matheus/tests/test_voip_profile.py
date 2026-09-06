#!/usr/bin/env python3
"""Emit tests of the installed renderer source with fake fs/UCI/ubus."""
import re
from pathlib import Path
root = Path(__file__).resolve().parent.parent
files = root / 'overlay/package/xr500v-voip/files'
shared = re.sub(r'^export ', '', (files/'accounts.uc').read_text(), flags=re.M)
renderer = re.sub(r'^(import .*;|#!.*)\n', '', (files/'voip-profile.uc').read_text(), flags=re.M)
renderer = re.sub(r'exit\(([0-9])\)', r'return \1', renderer)
prefix = r'''
let db={}, out={}, active=true, ip='10.10.10.2', custom=false;
let ARGV=['1'];
function check(v,label){if(!v){warn('FAIL '+label+'\n');exit(1);}}
function cursor(){return {get:function(p,s,k){return db[k];}};}
function connect(){return {call:function(o,m,a){check(o=='network.interface.voice','selected network only');return {up:active,l3_device:'eth1.3','ipv4-address':ip?[{address:ip}]:[]};}};}
function readfile(p){return 'module xr500v.so\nsip_listen 0.0.0.0:5060\nnet_interface br-lan\nsip_transports udp,tcp\nctrl_tcp_listen 127.0.0.1:4444\n';}
function writefile(p,s){out[p]=s;return length(s);}
function chmod(p,m){check(m==384,'private runtime');return true;}
function unlink(p){out[p]='REMOVED';return true;}
function stat(p){return custom?{type:'file'}:null;}
'''
suffix = r'''
check(render()==0 && length(out)==0,'legacy no-op');
for(let k,v in account_defaults)db[k]=v;
db.account_mode='provider';db.network='voice';db.username='100';db.domain='sip.example.invalid';db.password='dummy';db.dial_mode='keypad';
render();
let path='/var/run/xr500v-voip/phone1/';
check(index(out[path+'config'],'sip_listen 10.10.10.2:5060')>=0,'bound IPv4');
check(index(out[path+'config'],'net_interface 10.10.10.2')>=0,'bound SDP address');
check(index(out[path+'config'],'0.0.0.0')<0 && index(out[path+'config'],'br-lan')<0,'old bindings removed');
check(index(out[path+'config'],'ctrl_tcp_listen 127.0.0.1:4444')>=0,'control untouched');
check(index(out[path+'accounts'],'sip:100@sip.example.invalid')>=0,'provider AoR');
check(out[path+'dialing']=='keypad\nprovider\nsip.example.invalid\n','bounded keypad config');
check(out[path+'speeddial']=='REMOVED','no competing hotline');
out={};active=false;check(render()==3 && length(out)==0,'down network no writes/fallback');
active=true;ip='';check(render()==3 && length(out)==0,'IPv4 required');ip='10.10.10.2';
db.password='bad\nvalue';check(render()==2 && length(out)==0,'invalid secret never written');db.password='dummy';
db.account_mode='custom';db.dial_mode='none';custom=true;out={};render();
check(out[path+'accounts']==null,'custom content untouched');
db.account_mode='local';db.dial_mode='hotline';db.hotline='sip:phone2@10.10.10.2:5062';out={};render();
check(index(out[path+'accounts'],'regint=0')>=0 && index(out[path+'accounts'],'dummy')<0,'local no credentials or registration');
check(out[path+'speeddial']==db.hotline+'\n','explicit hotline');
print('PASS: actual profile renderer, binding, legacy/custom preservation, no fallback, credentials, keypad/hotline generation\n');
'''
print(prefix+shared+'\nfunction render(){\n'+renderer+'\n}\n'+suffix)
