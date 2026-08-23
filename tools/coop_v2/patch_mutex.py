import shutil, os

exe = r'E:\G-Force\GForce.exe'
exe_bak = r'E:\G-Force\GForce.exe.bak'
patch_offset = 0x213C3C
old_byte = 0x75
new_byte = 0xEB

if not os.path.exists(exe_bak):
    shutil.copy2(exe, exe_bak)
    print(f'Backup created: {exe_bak}')

with open(exe, 'rb') as f:
    data = bytearray(f.read())

if data[patch_offset] != old_byte:
    print(f'ERROR: byte at 0x{patch_offset:X} is 0x{data[patch_offset]:02X}, expected 0x{old_byte:02X}')
    exit(1)

data[patch_offset] = new_byte

with open(exe, 'wb') as f:
    f.write(data)

print(f'PATCHED: VA 0x613C3C offset 0x{patch_offset:X}: jne -> jmp (75->EB)')
print(f'Second instance mutex check bypassed!')
