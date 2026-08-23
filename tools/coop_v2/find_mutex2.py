import struct

with open(r'E:\G-Force\GForce.exe', 'rb') as f:
    data = f.read()

pe_off = struct.unpack_from('<I', data, 0x3C)[0]
nt_off = pe_off + 4 + 20
imp_rva = struct.unpack_from('<I', data, nt_off + 104)[0]
nsec = struct.unpack_from('<H', data, pe_off + 6)[0]
opt_size = struct.unpack_from('<H', data, pe_off + 20)[0]
sec_off = pe_off + 24 + opt_size

def rva_to_file(rva):
    for i in range(nsec):
        s = sec_off + i * 40
        va = struct.unpack_from('<I', data, s + 12)[0]
        vsize = struct.unpack_from('<I', data, s + 8)[0]
        rawptr = struct.unpack_from('<I', data, s + 20)[0]
        if va <= rva < va + vsize:
            return rawptr + (rva - va)
    return 0

# Find CreateMutexA IAT
d_off = rva_to_file(imp_rva)
mutex_iat = 0
while True:
    ilt = struct.unpack_from('<I', data, d_off)[0]
    name_rva = struct.unpack_from('<I', data, d_off + 12)[0]
    iat_rva = struct.unpack_from('<I', data, d_off + 16)[0]
    if ilt == 0 and name_rva == 0: break
    mn = data[rva_to_file(name_rva):rva_to_file(name_rva)+64].split(b'\x00')[0]
    if b'KERNEL32' in mn.upper():
        ilt_off = rva_to_file(ilt) if ilt else 0
        iat_off = rva_to_file(iat_rva) if iat_rva else 0
        if ilt_off and iat_off:
            j = 0
            while True:
                th = struct.unpack_from('<I', data, ilt_off + j*4)[0]
                if th == 0: break
                if not (th & 0x80000000):
                    fo = rva_to_file(th & 0x7FFFFFFF)
                    fn = data[fo+2:fo+66].split(b'\x00')[0].decode('ascii','replace')
                    ia = struct.unpack_from('<I', data, iat_off + j*4)[0]
                    if 'CreateMutex' in fn:
                        mutex_iat = ia
                        print(f'CreateMutexA IAT: 0x{ia:08X}')
                j += 1
    d_off += 20

# Find all FF 15 calls to CreateMutexA
if mutex_iat:
    pat = b'\xFF\x15' + struct.pack('<I', mutex_iat)
    pos = 0
    while True:
        idx = data.find(pat, pos)
        if idx < 0: break
        print(f'Call CreateMutexA at 0x{0x400000+idx:08X}')
        s = max(0, idx - 80)
        for j in range(s, min(idx+32, len(data)), 16):
            h = ' '.join(f'{data[k]:02X}' for k in range(j, min(j+16, len(data))))
            print(f'  0x{0x400000+j:08X}: {h}')
        pos = idx + 6

# Also search for push "mutex" string (VA 0x00720219)
print('\n--- Push "mutex" string ---')
pat2 = b'\x68' + struct.pack('<I', 0x00720219)
pos = 0
while True:
    idx = data.find(pat2, pos)
    if idx < 0: break
    print(f'Push 0x00720219 at 0x{0x400000+idx:08X}')
    s = max(0, idx - 32)
    for j in range(s, min(idx+32, len(data)), 16):
        h = ' '.join(f'{data[k]:02X}' for k in range(j, min(j+16, len(data))))
        print(f'  0x{0x400000+j:08X}: {h}')
    pos = idx + 5

# Search for "An instance of %s is already running" (VA 0x00720205)
print('\n--- Push "already running" string ---')
pat3 = b'\x68' + struct.pack('<I', 0x00720205)
pos = 0
while True:
    idx = data.find(pat3, pos)
    if idx < 0: break
    print(f'Push 0x00720205 at 0x{0x400000+idx:08X}')
    s = max(0, idx - 48)
    for j in range(s, min(idx+48, len(data)), 16):
        h = ' '.join(f'{data[k]:02X}' for k in range(j, min(j+16, len(data))))
        print(f'  0x{0x400000+j:08X}: {h}')
    pos = idx + 5

# Search for GetLastError (to find the check after CreateMutexA)
print('\n--- Search for "Error closing app mutex" ---')
pat4 = b'\x68' + struct.pack('<I', 0x007201E2)
pos = 0
while True:
    idx = data.find(pat4, pos)
    if idx < 0: break
    print(f'Push 0x007201E2 at 0x{0x400000+idx:08X}')
    s = max(0, idx - 48)
    for j in range(s, min(idx+48, len(data)), 16):
        h = ' '.join(f'{data[k]:02X}' for k in range(j, min(j+16, len(data))))
        print(f'  0x{0x400000+j:08X}: {h}')
    pos = idx + 5
