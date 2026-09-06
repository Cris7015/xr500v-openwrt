#!/usr/bin/env python3
"""Synthetic TrendChip header tests. No real firmware or flash writes."""
import importlib.util
from pathlib import Path
import lzma
import struct
import subprocess
import sys
import tempfile

path=Path(__file__).resolve().parent.parent/'overlay/target/linux/airoha/image/xr500v-trendchip-header.py'
spec=importlib.util.spec_from_file_location('xr_header',path)
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)

with tempfile.TemporaryDirectory(prefix='xr500v-header-test-') as temp:
    file=Path(temp)/'synthetic.img'
    stream=lzma.compress(b'SYNTHETIC-NOT-A-KERNEL'*256,format=lzma.FORMAT_ALONE)
    squashfs=bytearray(128);squashfs[:4]=b'hsqs';struct.pack_into('<Q',squashfs,40,len(squashfs))
    payload=stream+bytes(module.KERNEL_PARTITION_SIZE-len(stream))+squashfs
    file.write_bytes(payload)
    subprocess.run([sys.executable,str(path),str(file)],check=True)
    wrapped=file.read_bytes()
    assert wrapped[0x60:0x68]==module.TRENDCHIP_MAGIC
    assert len(wrapped)==len(payload)+512 and wrapped[512:]==payload
    for bad in [b'truncated',b'NOT-LZMA'+payload[8:],payload[:-1],payload[:module.KERNEL_PARTITION_SIZE]+b'bad!'+payload[module.KERNEL_PARTITION_SIZE+4:]]:
        file.write_bytes(bad)
        result=subprocess.run([sys.executable,str(path),str(file)],capture_output=True)
        assert result.returncode!=0 and file.read_bytes()==bad
print('PASS: synthetic wrapper geometry/payload preservation and fail-closed truncated/invalid images')
