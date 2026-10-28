"""Integration tests for the real C image tool; no third-party packages."""
import os
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOL = Path(os.environ.get('EDUFS_TOOL', str(ROOT / 'build' / ('edufs.exe' if os.name == 'nt' else 'edufs'))))
BLOCK = 4096
MAX_FILE = (12 + 1024) * BLOCK


class Images(unittest.TestCase):
    def setUp(self):
        (ROOT / 'build').mkdir(exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(dir=ROOT / 'build')
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.image = self.base / 'disk.img'
        self.run_tool('mkfs', self.image, 2048)

    def run_tool(self, *args, ok=True):
        result = subprocess.run([str(TOOL), *map(str, args)], capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, ok, result.stderr)
        return result

    def put(self, name, data, ok=True):
        source = self.base / 'input.bin'
        source.write_bytes(data)
        return self.run_tool('put', self.image, name, source, ok=ok)

    def get(self, name):
        with tempfile.TemporaryDirectory(dir=self.base) as directory:
            output = Path(directory) / 'output.bin'
            self.run_tool('get', self.image, name, output)
            return output.read_bytes()

    def test_boundaries(self):
        rng = random.Random(21)
        for size in [0, 1, BLOCK-1, BLOCK, BLOCK+1, 12*BLOCK-1,
                     12*BLOCK, 12*BLOCK+1, 13*BLOCK, MAX_FILE]:
            with self.subTest(size=size):
                data = rng.randbytes(size)
                self.put('boundary', data)
                self.assertEqual(self.get('boundary'), data)
                self.run_tool('check', self.image)
        before = self.image.read_bytes()
        self.put('boundary', b'x' * (MAX_FILE+1), ok=False)
        self.assertEqual(self.image.read_bytes(), before)

    def test_disk_full_preserves_old_file(self):
        small = self.base / 'small.img'
        self.run_tool('mkfs', small, 24)  # 15 usable blocks
        self.image = small
        original = b'old contents'
        self.put('existing', original)
        self.put('filler', b'f' * (12*BLOCK))
        before = self.image.read_bytes()
        self.put('existing', b'n' * (13*BLOCK), ok=False)
        self.assertEqual(self.image.read_bytes(), before)
        self.assertEqual(self.get('existing'), original)
        self.run_tool('rm', self.image, 'filler')
        self.put('existing', b'n' * (13*BLOCK))
        self.assertEqual(self.get('existing'), b'n' * (13*BLOCK))
        self.run_tool('check', self.image)

    def test_failure_after_indirect_allocation(self):
        small = self.base / 'tiny.img'
        self.run_tool('mkfs', small, 22)  # 13 free: indirect + 12 data, fails on 13th
        self.image = small
        before = small.read_bytes()
        self.put('too-big', b'x' * (13*BLOCK), ok=False)
        self.assertEqual(small.read_bytes(), before)
        self.run_tool('check', small)

    def test_inode_exhaustion_and_reuse(self):
        for n in range(126):
            self.put(f'f{n}', b'')
        before = self.image.read_bytes()
        self.put('overflow', b'', ok=False)
        self.assertEqual(self.image.read_bytes(), before)
        self.run_tool('rm', self.image, 'f60')
        self.put('reused', b'hello')
        self.assertEqual(self.get('reused'), b'hello')

    def test_seeded_operations_with_reopen(self):
        rng = random.Random(4523)
        model = {}
        for _ in range(160):
            name = f'file{rng.randrange(12)}'
            if name in model and rng.randrange(4) == 0:
                self.run_tool('rm', self.image, name)
                del model[name]
            else:
                data = rng.randbytes(rng.choice([0, 19, BLOCK-1, 12*BLOCK+7, 20*BLOCK]))
                self.put(name, data)
                model[name] = data
            self.run_tool('check', self.image)
            for name, expected in model.items():
                self.assertEqual(self.get(name), expected)

    def test_corruption_rejected_before_mutation(self):
        self.put('a', b'a'*(13*BLOCK))
        self.put('b', b'b')
        good = self.image.read_bytes()
        inode2 = 3*BLOCK + 2*128
        inode3 = 3*BLOCK + 3*128
        indirect = struct.unpack_from('<I', good, inode2+56)[0]
        first = struct.unpack_from('<I', good, inode2+8)[0]
        cases = [(0, 0), (4, 99), (12, 9999), (inode2+8, 50000),
                 (inode2+8, 1), (inode3+8, first),
                 (indirect*BLOCK, first), (inode2+4, MAX_FILE+1),
                 (inode2+56, 0)]
        for offset, value in cases:
            with self.subTest(offset=offset, value=value):
                corrupt = bytearray(good)
                struct.pack_into('<I', corrupt, offset, value)
                self.image.write_bytes(corrupt)
                self.run_tool('check', self.image, ok=False)
                self.put('new', b'new', ok=False)
                self.assertEqual(self.image.read_bytes(), corrupt)
        self.image.write_bytes(good)
        corrupt = bytearray(good)
        corrupt[BLOCK + first//8] ^= 1 << (first%8)
        self.image.write_bytes(corrupt)
        self.run_tool('check', self.image, ok=False)

    def test_names_and_existing_output_protection(self):
        before = self.image.read_bytes()
        for name in ['', '.', '..', 'a/b', 'x'*60]:
            self.put(name, b'a', ok=False)
            self.assertEqual(self.image.read_bytes(), before)
        self.put('x'*59, b'valid')
        self.run_tool('mkfs', self.image, 2048, ok=False)
        self.run_tool('get', self.image, 'x'*59, self.image, ok=False)
        self.assertEqual(self.get('x'*59), b'valid')

    def test_truncated_image(self):
        self.image.write_bytes(self.image.read_bytes()[:-1])
        self.run_tool('check', self.image, ok=False)


if __name__ == '__main__':
    unittest.main(verbosity=2)
