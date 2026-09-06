#!/usr/bin/env python3
"""Offline algorithm checks; NOT hardware or kernel integration validation."""
import random

rng = random.Random(505309)
for size in (14, 60, 64, 512, 1514, 1518):
    for vif in (0, 3, 8, 15):
        frame = bytes(rng.randrange(256) for _ in range(size))
        tagged = bytearray(b"\0" * 4 + frame)
        tagged[:12] = tagged[4:16]
        tagged[12:14] = b"\x81\x00"
        tagged[14:16] = vif.to_bytes(2, "big")
        assert tagged[:12] == frame[:12]
        assert tagged[16:] == frame[12:]
        tagged[4:16] = tagged[:12]
        assert bytes(tagged[4:]) == frame

# The final sentinel descriptor cannot be consumed. WHNAT preserves32 free
# entries, while ordinary TX can still use the rest; completions may reorder.
for capacity in (64, 128, 256):
    free = set(range(capacity))
    busy = set()
    for _ in range(10000):
        if busy and rng.randrange(3) == 0:
            slot = rng.choice(sorted(busy))
            busy.remove(slot)
            free.add(slot)
        else:
            whnat = bool(rng.randrange(2))
            if len(free) > (32 if whnat else 1):
                slot = min(free)
                free.remove(slot)
                busy.add(slot)
                if whnat: assert len(free) >= 32
        assert not free.intersection(busy)
        assert len(free) + len(busy) == capacity
        assert len(free) >= 1

# Backend's positive EBUSY is consumed. Positive DMA-map error is not.
for code, consumed in ((0, True), (16, True), (-16, False), (-11, False), (1, False)):
    assert (code in (0, 16)) == consumed
print("PASS: tag/rollback byte preservation, descriptor-reserve model, TX result contract")
