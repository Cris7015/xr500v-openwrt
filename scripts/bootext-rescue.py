#!/usr/bin/env python3
"""BootROM rescue of the Archer XR500v (EN751221) with bootext.bin, over UART.

Power the router on with RESET held: the BootROM calibrates the DRAM, then
prints "done" every ~25 s. This script types "x" until the ROM takes it,
sends bootext.bin (DRAM calibration and an XMODEM receiver) by 1K-XMODEM,
picks "x" (chainload) in the bootext menu -- never "b", which writes the
flash -- sends u-boot.bin, and stops U-Boot's autoboot. It leaves U-Boot at
its prompt and exits; open a terminal on the same port (115200 8N1) to use
it. Nothing is written to the NAND.

The BootROM sometimes throws "Undefined Exception" right after a transfer;
the script then waits for the next cycle and sends again.

usage: bootext-rescue.py PORT BOOTEXT UBOOT LOG
env:   ROM_WAIT  seconds to wait for each "done" (default 110)
       ROM_ATTEMPTS  transfers to try before giving up (default 3)
To leave the rescue, cut the power: a reset from software returns to the ROM.
"""
import atexit
import fcntl
import os
import select
import sys
import termios
import time

if len(sys.argv) != 5:
    sys.exit(__doc__)
port, bootext, uboot, logf = sys.argv[1:5]
os.system(f"stty -F {port} 115200 cs8 -cstopb -parenb raw -echo")
fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
fcntl.ioctl(fd, termios.TIOCEXCL)
atexit.register(lambda: fcntl.ioctl(fd, termios.TIOCNXCL))
log = open(logf, "ab")

SOH, STX, EOT, ACK, NAK, CAN = b"\x01", b"\x02", b"\x04", b"\x06", b"\x15", b"\x18"


def write_all(data):
    end = time.monotonic() + 8
    while data:
        if time.monotonic() > end:
            raise TimeoutError("UART TX stalled")
        if select.select([], [fd], [], 0.2)[1]:
            try:
                n = os.write(fd, data)
            except BlockingIOError:
                continue
            data = data[n:]


def rd(t, stop=()):
    buf = b""
    end = time.time() + t
    while time.time() < end:
        if select.select([fd], [], [], 0.02)[0]:
            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                chunk = b""
            if chunk:
                buf += chunk
                log.write(chunk)
                log.flush()
        if any(s in buf for s in stop):
            break
    return buf


def log_mark():
    log.flush()
    return os.path.getsize(logf)


def wait_log(mark, pats, t, poke=None):
    """Wait for one of pats in everything logged since mark: text that an
    earlier read already consumed (XMODEM acknowledgements) still counts."""
    end = time.time() + t
    while True:
        log.flush()
        with open(logf, "rb") as f:
            f.seek(mark)
            data = f.read()
        if any(p in data for p in pats) or time.time() > end:
            return data
        if poke:
            write_all(poke)
        rd(0.1)


def crc16(data):
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def xmodem(path, name, handshake_seen=False, wait=30):
    data = open(path, "rb").read()
    if not handshake_seen:
        hs = b""
        end = time.time() + wait
        while b"C" not in hs and time.time() < end:
            hs += rd(0.5)
        if b"C" not in hs:
            print(f"{name}: no 1K-XMODEM/CRC request", flush=True)
            return False
    print(f"{name}: sending {len(data)} bytes", flush=True)
    blk, pos, errors = 1, 0, 0
    while pos < len(data):
        chunk = data[pos:pos + 1024].ljust(1024, b"\x1a")
        write_all(STX + bytes([blk & 0xFF, 0xFF - (blk & 0xFF)]) + chunk +
                  crc16(chunk).to_bytes(2, "big"))
        reply = b""
        end = time.time() + 6
        while time.time() < end and reply[-1:] not in (ACK, NAK, CAN):
            reply += rd(0.05)
        # The ROM queues old 'C'/NAK bytes before it answers: the last byte counts.
        if reply[-1:] == ACK:
            blk, pos, errors = blk + 1, pos + 1024, 0
        elif reply[-1:] == CAN:
            print(f"{name}: cancelled by the receiver", flush=True)
            return False
        else:
            errors += 1
            if errors > 8:
                print(f"{name}: too many errors", flush=True)
                return False
    for _ in range(5):
        write_all(EOT)
        if ACK in rd(3):
            print(f"{name}: done, {blk - 1} blocks", flush=True)
            return True
    print(f"{name}: no acknowledgement of the end of transfer", flush=True)
    return False


def rom_takes_bootext():
    """The ROM only takes the key in the ~20 s of silence before it prints
    "done"; the 'C' that asks for XMODEM must follow right after."""
    print('Waiting for the BootROM (power on with RESET held)...', flush=True)
    buf = b""
    end = time.time() + int(os.environ.get("ROM_WAIT", "110"))
    while time.time() < end:
        os.write(fd, b"x")
        buf = (buf + rd(0.3))[-4096:]
        # "done" alone on its line; "DRAMC init done." and the C of
        # "Calculate size" must not count.
        i = buf.find(b"\ndone")
        if i < 0:
            continue
        tail = buf[i + 5:]
        buf = b""
        until = time.time() + 3
        while time.time() < until and not tail.lstrip(b"\r\n"):
            tail += rd(0.1)
        if not tail.lstrip(b"\r\n").startswith(b"C"):
            continue
        mark = log_mark()
        if not xmodem(bootext, "bootext.bin", handshake_seen=True):
            sys.exit(4)
        verdict = wait_log(mark, (b"jump to", b"Undefined Exception"), 5)
        if b"Undefined Exception" in verdict.split(b"jump to", 1)[0]:
            print("The BootROM threw an exception after the transfer; next cycle.", flush=True)
            return None
        return mark
    print("The BootROM never took the key: was RESET held at power-on?", flush=True)
    sys.exit(2)


for _ in range(int(os.environ.get("ROM_ATTEMPTS", "3"))):
    mark = rom_takes_bootext()
    if mark is not None:
        break
else:
    sys.exit("The BootROM rejected every transfer; power-cycle with RESET held and retry.")

menu = wait_log(mark, (b"Press x to chainload", b"chainload selected"), 120)
if b"Press x to chainload" not in menu and b"chainload selected" not in menu:
    sys.exit("bootext.bin did not reach its menu; see the log.")
if b"chainload selected" not in menu:
    write_all(b"x")                     # chainload; never "b" (flash)
if b"chainload selected" not in wait_log(mark, (b"chainload selected",), 8):
    sys.exit("bootext.bin did not confirm the chainload; see the log.")

umark = log_mark()
if not xmodem(uboot, "u-boot.bin"):
    sys.exit(5)
# Space every 0.1 s until the prompt: the countdown is one second.
out = wait_log(umark, (b"U-Boot> ",), 60, poke=b" ")
if b"U-Boot> " not in out:
    sys.exit("U-Boot did not reach its prompt; see the log.")
m = log_mark()
write_all(b"\x03")
wait_log(m, (b"U-Boot> ",), 5)
for line in out.decode(errors="replace").replace("\r", "").splitlines():
    if line.startswith(("U-Boot 20", "DRAM:", "spi-nand")):
        print(line, flush=True)
print(f"U-Boot is at its prompt in RAM. Open a terminal on {port} at 115200.", flush=True)
