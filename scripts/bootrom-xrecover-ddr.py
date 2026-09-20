#!/usr/bin/env python3
"""Recovery del BootROM EN751221 (doc tcboot.bin.md): esperar "done", teclear 'x', subir el chainloader por 1K-XMODEM,
luego u-boot.bin al chainloader. uso: bootrom-xrecover.py PORT CHAINLOADER UBOOT LOG"""
import os, sys, time, select, fcntl, termios
dev, chain, uboot, logf = sys.argv[1:5]
fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK); log = open(logf, 'ab')
fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
fcntl.ioctl(fd, termios.TIOCEXCL)
import atexit
atexit.register(lambda: fcntl.ioctl(fd, termios.TIOCNXCL))
def write_all(data):
    end=time.monotonic()+8
    while data:
        if time.monotonic()>end: raise TimeoutError('UART TX stalled')
        if select.select([], [fd], [], .2)[1]:
            try:n=os.write(fd,data)
            except BlockingIOError:continue
            data=data[n:]
SOH, STX, EOT, ACK, NAK, CAN = b'\x01', b'\x02', b'\x04', b'\x06', b'\x15', b'\x18'
def rd(t, stop=()):
    b = b''; d = time.time() + t
    while time.time() < d:
        if select.select([fd], [], [], 0.02)[0]:
            try: c = os.read(fd, 4096)
            except BlockingIOError: c = b''
            if c: b += c; log.write(c); log.flush()
        if any(s in b for s in stop): break
    return b
def crc16(b):
    c = 0
    for x in b:
        c ^= x << 8
        for _ in range(8): c = ((c << 1) ^ 0x1021) & 0xffff if c & 0x8000 else (c << 1) & 0xffff
    return c
def xsend(path, onek, hs_wait=30, tag='', crc_seen=False):
    data = open(path, 'rb').read(); hs = b''; t0 = time.time(); crc = True if crc_seen else None
    while crc is None and time.time() - t0 < hs_wait:
        hs += rd(0.5)
        if b'C' in hs: crc = True; break
        if NAK in hs: crc = False; break
    if crc is None: print(f'    {tag}: sin handshake en {hs_wait}s: {hs[-120:]!r}', flush=True); return False
    print(f'    {tag}: handshake {"CRC" if crc else "checksum"} tras {round(time.time()-t0,1)}s; enviando {len(data)} B en bloques de {1024 if onek else 128}', flush=True)
    bs = 1024 if onek else 128; blk = 1; pos = 0; errs = 0
    while pos < len(data):
        chunk = data[pos:pos+bs].ljust(bs, b'\x1a')
        frame = (STX if onek else SOH) + bytes([blk & 0xff, 0xff - (blk & 0xff)]) + chunk + (crc16(chunk).to_bytes(2, 'big') if crc else bytes([sum(chunk) & 0xff]))
        write_all(frame)
        r = b''; d = time.time() + 6
        while time.time() < d and (not r or r[-1:] not in (ACK, NAK, CAN)): r += rd(0.05)
        # el ROM encola 'C'/NAK viejos antes de contestar: vale el ÚLTIMO byte
        v = r[-1:]
        if v == ACK: blk += 1; pos += bs; errs = 0
        elif v == NAK: errs += 1
        elif v == CAN: print(f'    CAN ({r[-20:]!r})', flush=True); return False
        else: errs += 1; print(f'    bloque {blk}: resp {r[:30]!r}', flush=True)
        if errs > 8: print('    demasiados errores', flush=True); return False
    eot_ack=False
    for _ in range(5):
        write_all(EOT); r = rd(3)
        if ACK in r:eot_ack=True;break
    if not eot_ack:
        print(f'    {tag}: missing EOT ACK',flush=True);return False
    print(f'    {tag}: EOT ok, {blk-1} bloques', flush=True); return True

# El ROM sólo toma la tecla durante el silencio de ~20 s ANTES de imprimir "done" (tras "MCC1="); después de "done"
# salta directo (Undefined Exception). Tecleamos 'x' cada 0,5 s hasta ver "done" y comprobamos que siguió "err qdma"/'C'.
def rom_take_chainloader():
    """Stages 1-3; False when the BootROM throws after receiving it (it does now and then)."""
    print('[1] tecleando "x" cada 0,5 s hasta que el ROM acepte (ciclo de ~25 s, hasta 4 ciclos)...', flush=True)
    ok = False; t0 = time.time()
    while time.time() - t0 < int(os.environ.get('ROM_WAIT', '110')) and not ok:
        os.write(fd, b'x'); out = rd(1.5, (b'done',))
        if b'done' in out:
            # mirar SOLO lo que viene después de "done": antes está "MCC1=" (tiene una C) y "DDR CALI" del ciclo anterior
            tail = out.split(b'done', 1)[1]; d = time.time() + 3
            while time.time() < d and b'C' not in tail and b'Undefined Exception' not in tail: tail += rd(0.1)
            if b'Undefined Exception' in tail or b'C' not in tail:
                print(f'    "done" sin tomar la x ({tail[:60]!r}); sigo con el próximo ciclo', flush=True); continue
            print(f'[2] "done" + C del ROM: {tail[:80]!r}', flush=True); ok = True
    if not ok: print('el ROM nunca tomó la x; ¿está en modo ROM?'); sys.exit(2)
    print('[3] chainloader por 1K-XMODEM', flush=True)
    if not xsend(chain, True, 30, 'chainloader', crc_seen=True): sys.exit(4)
    # xsend() may already have read the BootROM's verdict into the log.
    log.flush()
    after = open(logf, 'rb').read().rsplit(b'received len', 1)[-1]
    if b'jump to' not in after and b'Undefined Exception' not in after:
        after += rd(3, (b'jump to', b'Undefined Exception'))
    if b'Undefined Exception' in after.split(b'jump to', 1)[0]:
        print('    el ROM tiró "Undefined Exception" tras recibir el chainloader; reintento en el próximo ciclo', flush=True)
        return False
    return True

for attempt in range(int(os.environ.get('ROM_ATTEMPTS', '3'))):
    if rom_take_chainloader(): break
else:
    print('el ROM rechazó el chainloader en todos los intentos'); sys.exit(8)

# The DDR-stage chainloader prints "DDR CALI"/"Calculate size" (capital C)
# before its XMODEM receiver starts: only accept the handshake after "waiting".
# xsend() already reads whatever follows the chainloader's EOT ACK, so the
# "waiting" line may have gone into the log rather than into `out`.
log.flush()
out = open(logf, 'rb').read()[-3000:]
if b'waiting' not in out:
    out += rd(90, (b'waiting', b'U-Boot>', b'refusing', b'failed'))
if b'waiting' not in out:
    print('    chainloader no llegó a "waiting":', out.decode(errors='replace').replace('\r','').strip()[-1500:], flush=True); sys.exit(7)
out += rd(4, (b'C',))
print('    tras el chainloader:', out.decode(errors='replace').replace('\r','').strip()[-1500:], flush=True)
print('[4] u-boot.bin al chainloader (1K, CRC)', flush=True)
if not xsend(uboot, True, 30, 'u-boot.bin', crc_seen=out.rstrip().endswith(b'C')): sys.exit(5)
out = rd(40, (b'U-Boot>',))
print('    salida:', out.decode(errors='replace').replace('\r','').strip()[-700:], flush=True)
if b'U-Boot>' in out: print('LISTO: U-Boot cargado por el BootROM + chainloader', flush=True); sys.exit(0)
print('sin prompt de U-Boot', flush=True); sys.exit(6)
