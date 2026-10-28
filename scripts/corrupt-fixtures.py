"""Create disposable invalid images for guest mount rejection tests."""
from pathlib import Path
import struct
import subprocess
import sys

build = Path(sys.argv[1])
tool = build / 'initramfs/bin/edufs'
for kind in ('super', 'pointer'):
    image = build / f'corrupt-{kind}.img'
    if image.exists():
        image.unlink()
    subprocess.run([str(tool), 'mkfs', str(image), '64'], check=True)
    if kind == 'pointer':
        source = build / 'corrupt-fixture-input.txt'
        source.write_bytes(b'mount must reject an invalid block reference')
        subprocess.run([str(tool), 'put', str(image), 'bad', str(source)], check=True)
    data = bytearray(image.read_bytes())
    struct.pack_into('<I', data, 0 if kind == 'super' else 3*4096+2*128+8,
                     0 if kind == 'super' else 0xffffffff)
    image.write_bytes(data)
