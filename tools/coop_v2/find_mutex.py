import struct, sys

with open(r'E:\G-Force\GForce.exe', 'rb') as f:
    data = f.read()

# Find all null-terminated ASCII strings that could be mutex names
# Common patterns: "Global\...", "Local\...", anything with Mutex/MutexA
results = []
for needle in [b'Mutex', b'mutex', b'MTX', b'mtx', b'GForce', b'G-Force', 
              b'SINGLE', b'SingleInst', b'PIPE', b'Global\\', b'Local\\',
              b'Already running', b'already running', b'only one']:
    pos = 0
    while True:
        idx = data.find(needle, pos)
        if idx < 0:
            break
        # Find surrounding null-terminated string
        start = idx
        while start > 0 and data[start-1] >= 0x20 and data[start-1] < 0x7F:
            start -= 1
        end = data.find(b'\x00', idx)
        if end < 0:
            end = idx + 64
        snippet = data[start:min(end, start + 128)]
        text = snippet.decode('ascii', 'replace')
        results.append((0x400000 + idx, text))
        pos = idx + len(needle)
        if len(results) > 50:
            break

for addr, text in sorted(set(results)):
    print(f'0x{addr:08X}: {text}')

# Also search for CreateMutexA xrefs - look for push + call pattern
# near typical API call sequences
print("\n--- Searching for CreateMutexA call pattern ---")
# IAT entry for CreateMutexA is at file offset that maps to VA 0x004F7B92
# But at file time, IAT contains stubs. Let's find the actual call sites
# by looking for push string / push 0 / call CreateMutexA pattern
# Since we know IAT VA = 0x004F7B92, search for FF 15 92 7B 4F 00
needle = bytes([0xFF, 0x15, 0x92, 0x7B, 0x4F, 0x00])
pos = 0
while True:
    idx = data.find(needle, pos)
    if idx < 0:
        break
    print(f'call [CreateMutexA] at VA 0x{0x400000+idx:08X}')
    pos = idx + 6

# Also search for indirect calls via other patterns
# Sometimes compiler uses: mov reg, addr; call reg
# Or: push args; call dword ptr [IAT]
# Let's also check for any call that references near the IAT area
print("\n--- All FF 15 calls to CreateMutexA area (0x4F7B8C-0x4F7B98) ---")
for target_va in [0x4F7B92]:
    # little-endian bytes of the target VA
    target_bytes = struct.pack('<I', target_va)
    pattern = b'\xFF\x15' + target_bytes
    pos = 0
    while True:
        idx = data.find(pattern, pos)
        if idx < 0:
            break
        print(f'  call [0x{target_va:08X}] at VA 0x{0x400000+idx:08X} (file 0x{idx:X})')
        # show context: 64 bytes before
        start = max(0, idx - 64)
        for j in range(start, min(idx + 16, len(data)), 16):
            hexb = ' '.join(f'{data[k]:02X}' for k in range(j, min(j+16, len(data))))
            print(f'    0x{0x400000+j:08X}: {hexb}')
        pos = idx + 6
