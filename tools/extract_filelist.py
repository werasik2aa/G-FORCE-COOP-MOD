import struct, os, re, sys

ROOT = r"E:\G-Force"
BIN = os.path.join(ROOT, "data", "Filelist.bin")
BLOB = os.path.join(ROOT, "data", "Filelist.000")
OUT = os.path.join(ROOT, "g_force", "extracted", "Filelist_v7")
HASHH = r"C:\Users\werasik2aa\Downloads\gforce-tools-master\gforce_hashcodes.h"

b = open(BIN, "rb").read()
blob = open(BLOB, "rb").read()

ver, full_size, count = struct.unpack("<III", b[0:12])
one, zero = struct.unpack("<HH", b[12:16])
string_table_offset = struct.unpack("<I", b[16:20])[0]
print("ver=%d full_size=%d count=%d one=%d zero=%d str_tab_off=%d" % (ver, full_size, count, one, zero, string_table_offset))
print("blob size on disk = %d" % len(blob))

# hashcode -> name map
hmap = {}
if os.path.exists(HASHH):
    for line in open(HASHH, encoding="utf-8", errors="ignore"):
        m = re.match(r"#define\s+(\w+)\s+0x([0-9A-Fa-f]+)", line)
        if m:
            hmap[int(m.group(2), 16)] = m.group(1)
    print("hash names loaded: %d" % len(hmap))

ENT = 28
base = 0x14
str_arr_pos = 0x10 + string_table_offset

def decode_str(i, pos_field):
    val = struct.unpack("<I", b[pos_field:pos_field+4])[0]
    cur = pos_field + val
    out = bytearray()
    j = 0
    while j < 400:
        sb = b[cur + j]
        db = (sb + 0x16 - i - j) & 0xFF
        if db == 0:
            break
        out.append(db)
        j += 1
    try:
        return out.decode("latin1")
    except Exception:
        return repr(bytes(out))

def entry(i):
    e = base + i*ENT
    len_, hashcode, ver2, ts, unk, loc_offset, loc_file = struct.unpack("<IIIIIII", b[e:e+28])
    return len_, hashcode, ver2, ts, unk, loc_offset, loc_file

print("\n--- first 15 entries (decoded path | hashname) ---")
for i in range(min(15, count)):
    len_, hashcode, ver2, ts, unk, loc_offset, loc_file = entry(i)
    path = decode_str(i, str_arr_pos + i*4)
    hn = hmap.get(hashcode, "")
    print("%3d  path=%-50r hash=%08x %s len=%d loc_off=%08x loc_file=%08x" % (i, path, hashcode, hn, len_, loc_offset, loc_file))

if "--extract" in sys.argv:
    os.makedirs(OUT, exist_ok=True)
    manifest = open(os.path.join(OUT, "_manifest.csv"), "w", encoding="utf-8")
    manifest.write("index,hashcode,hashname,len,loc_offset,loc_file,path\n")
    total = 0
    for i in range(count):
        len_, hashcode, ver2, ts, unk, loc_offset, loc_file = entry(i)
        path = decode_str(i, str_arr_pos + i*4)
        hn = hmap.get(hashcode, "")
        if len_ == 0 or loc_offset + len_ > len(blob):
            # skip obviously bad
            manifest.write("%d,%08x,%s,%d,%08x,%08x,%s (SKIP)\n" % (i, hashcode, hn, len_, loc_offset, loc_file, path))
            continue
        data = blob[loc_offset:loc_offset+len_]
        total += len(data)
        # build output path
        safe = re.sub(r'^[a-zA-Z]:[\\/]', '', path.replace("\\", "/"))
        if not safe or safe == ".":
            safe = "0x%08x" % hashcode
        op = os.path.join(OUT, safe.lstrip("/"))
        os.makedirs(os.path.dirname(op) or OUT, exist_ok=True)
        open(op, "wb").write(data)
        manifest.write("%d,%08x,%s,%d,%08x,%08x,%s\n" % (i, hashcode, hn, len_, loc_offset, loc_file, path))
    manifest.close()
    print("\nextracted total bytes=%d (%.1f MB); blob=%.1f MB" % (total, total/1e6, len(blob)/1e6))
    print("output dir: %s" % OUT)
