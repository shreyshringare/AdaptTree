#!/usr/bin/env python3
"""Generate deterministic binary seed inputs for the differential fuzzer.

Wire format: sequence of 17-byte operation structs.
  struct Op { uint8_t op_type; uint64_t key; uint64_t value; }
  All multi-byte fields are little-endian.

Op type encoding:
  0x01 = INSERT  (key and value used)
  0x02 = GET     (key used; value ignored)
  0x03 = FULL_SCAN (key and value ignored)
"""

import struct
import os

# Output directory: fuzz/corpus/ relative to this script's location
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

def pack_op(op_type: int, key: int, value: int) -> bytes:
    """Pack a single 17-byte operation struct."""
    return struct.pack('<BQQ', op_type, key, value)

OP_INSERT    = 0x01
OP_GET       = 0x02
OP_FULL_SCAN = 0x03


def seed_001() -> bytes:
    """5 INSERTs with keys 1..5, value=key*10; then 1 FULL_SCAN."""
    ops = []
    for k in range(1, 6):
        ops.append(pack_op(OP_INSERT, k, k * 10))
    ops.append(pack_op(OP_FULL_SCAN, 0, 0))
    return b''.join(ops)


def seed_002() -> bytes:
    """3 INSERTs (keys 100, 200, 300; values 1000, 2000, 3000);
    3 GETs on those same keys; 1 FULL_SCAN."""
    ops = []
    pairs = [(100, 1000), (200, 2000), (300, 3000)]
    for k, v in pairs:
        ops.append(pack_op(OP_INSERT, k, v))
    for k, v in pairs:
        ops.append(pack_op(OP_GET, k, 0))
    ops.append(pack_op(OP_FULL_SCAN, 0, 0))
    return b''.join(ops)


def seed_003() -> bytes:
    """1 GET on a key that was never inserted (key=999); 1 FULL_SCAN.
    Tests empty-result agreement between both engines."""
    ops = []
    ops.append(pack_op(OP_GET, 999, 0))
    ops.append(pack_op(OP_FULL_SCAN, 0, 0))
    return b''.join(ops)


def main():
    seeds = {
        'seed_001.bin': seed_001(),
        'seed_002.bin': seed_002(),
        'seed_003.bin': seed_003(),
    }
    for name, data in seeds.items():
        path = os.path.join(SCRIPT_DIR, name)
        with open(path, 'wb') as f:
            f.write(data)
        print(f'Wrote {path} ({len(data)} bytes, {len(data) // 17} ops)')


if __name__ == '__main__':
    main()
