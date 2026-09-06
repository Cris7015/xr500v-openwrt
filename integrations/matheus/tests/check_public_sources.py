#!/usr/bin/env python3
"""Privacy/release guard for this selected export. Reports paths, never matches.

Heuristic checks supplement manual review; they cannot certify all history.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
SELF = Path(__file__).resolve()
errors = []
rules = {
    'private key': rb'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----',
    'service token': rb'(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,})',
    'subscriber serial text': rb'\b(?:MSTC|HWTC|ALCL|TPLG|TPLK|ZTEG|FHTT)[0-9A-Fa-f]{8}\b',
    'embedded subscriber property': rb'^\s*[+]?\s*gpon-serial-number\s*=',
    'machine-specific home': rb'/home/(?!user\b|example\b)[a-zA-Z0-9_-]+/|/mnt/c/Users/|C:\\Users\\',
    'private fixture LAN': rb'192\.168\.(?:68|60)\.[0-9]+',
}
for p in sorted(ROOT.rglob('*')):
    if '.git' in p.parts or '__pycache__' in p.parts:
        continue
    if p.is_symlink():
        errors.append((p, 'symlink')); continue
    if not p.is_file() or p.resolve() == SELF:
        continue
    if p.suffix.lower() in {'.bin','.ko','.apk','.ipk','.pcap','.pcapng','.raw','.wav','.mp3','.zip','.gz','.tgz','.pyc'}:
        errors.append((p, 'binary/private artifact')); continue
    data = p.read_bytes()
    if b'\0' in data:
        errors.append((p, 'binary data')); continue
    for label, pattern in rules.items():
        if re.search(pattern, data, re.M): errors.append((p,label))
    if p.suffix == '.dts' and b'&xpon {' in data:
        node = data.split(b'&xpon {',1)[1].split(b'\n};',1)[0]
        if b'status = "disabled";' not in node: errors.append((p,'public xPON default not disabled'))
for p,label in errors:
    print(f'FAIL {p.relative_to(ROOT)}: {label}')
if errors: sys.exit(1)
print('PASS: selected public export has no forbidden artifacts or detected identity/token/home patterns')
