import os, re, struct, sys

DATA = r"E:\G-Force\data"
OUT  = r"E:\G-Force\g_force\extracted"

LEN_BASE, HASH_BASE, OFF_BASE, STRIDE = 0x14, 0x18, 0x28, 28

# ---- .txt manifest (File_COM only) ----
txt_re = re.compile(
    r'^\s*(.+?)\s*:\s*Len\s+(\d+)\s*:\s*Ver\s+(\d+)\s*:\s*Hash\s+(0x[0-9a-fA-F]+)'
    r'\s*:\s*Ts\s+(0x[0-9a-fA-F]+)\s*:\s*Loc\s+([0-9a-fA-F]+):([0-9a-fA-F]+)\s*$'
)
def sanitize(name):
    name = name.replace('\\', '/')
    for c in '<>:"/\\|?*':
        name = name.replace(c, '_')
    return name

def entries_from_txt(path):
    out = []
    for ln in open(path, encoding='utf-8', errors='replace'):
        m = txt_re.match(ln.rstrip('\r\n'))
        if not m: continue
        p, length, ver, h, ts, hi, lo = m.groups()
        off = int(hi, 16) | (int(lo, 16) << 32)
        out.append({'name': sanitize(p), 'off': off, 'len': int(length), 'hash': h})
    return out

# ---- .bin binary manifest (all archives) ----
def entries_from_bin(path):
    b = open(path, 'rb').read()
    ver   = struct.unpack('<I', b[0:4])[0]
    bsize = struct.unpack('<I', b[4:8])[0]
    count = struct.unpack('<I', b[8:12])[0]
    lens  = [struct.unpack('<I', b[LEN_BASE+STRIDE*k:LEN_BASE+STRIDE*k+4])[0] for k in range(count)]
    hashs = [struct.unpack('<I', b[HASH_BASE+STRIDE*k:HASH_BASE+STRIDE*k+4])[0] for k in range(count)]
    offs  = [struct.unpack('<I', b[OFF_BASE+STRIDE*k:OFF_BASE+STRIDE*k+4])[0] for k in range(count)]
    out = []
    for k in range(count):
        out.append({'name': None, 'off': offs[k], 'len': lens[k],
                    'hash': '0x%08x' % hashs[k]})
    return out, bsize

def extract_all(entries, blob, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    done = 0; total = 0
    with open(blob, 'rb') as bf:
        for e in entries:
            if e['len'] <= 0:
                continue
            name = e['name'] or (e['hash'] + '.bin')
            dest = os.path.join(out_dir, name)
            # keep hash as subfolder for unnamed
            if e['name'] is None:
                dest = os.path.join(out_dir, e['hash'] + '.bin')
            else:
                os.makedirs(os.path.dirname(dest), exist_ok=True) if os.path.dirname(dest) else None
            bf.seek(e['off'])
            data = bf.read(e['len'])
            if len(data) != e['len']:
                print(f"  WARN short {name}: {len(data)}/{e['len']}")
            with open(dest, 'wb') as o:
                o.write(data)
            done += 1; total += e['len']
    return done, total

if __name__ == '__main__':
    archives = [
        ("File_COM", r"E:\G-Force\data\File_COM.txt", r"E:\G-Force\data\File_COM.000", os.path.join(OUT, "File_COM_named")),
        ("Filelist", None,                              r"E:\G-Force\data\Filelist.000", os.path.join(OUT, "Filelist_raw")),
        ("File_RUS", None,                             r"E:\G-Force\data\File_RUS.000", os.path.join(OUT, "File_RUS_raw")),
    ]
    summary = []
    for name, txt, blob, odir in archives:
        if txt and os.path.exists(txt):
            ents = entries_from_txt(txt)
        else:
            ents, bsz = entries_from_bin(os.path.join(DATA, name + ".bin"))
            blobsize = os.path.getsize(blob)
            print(f"[{name}] bin count={len(ents)} sum(len)={sum(e['len'] for e in ents)} blob={blobsize} match={sum(e['len'] for e in ents)==blobsize}")
        n, t = extract_all(ents, blob, odir)
        summary.append((name, n, t))
        print(f"[{name}] extracted {n} files, {t:,} bytes -> {odir}")
