import os, re, struct, sys

DATA = r"E:\G-Force\data"

line_re = re.compile(
    r'^\s*(.+?)\s*:\s*Len\s+(\d+)\s*:\s*Ver\s+(\d+)\s*:\s*Hash\s+(0x[0-9a-fA-F]+)'
    r'\s*:\s*Ts\s+(0x[0-9a-fA-F]+)\s*:\s*Loc\s+([0-9a-fA-F]+):([0-9a-fA-F]+)\s*$'
)

def parse_manifest(path):
    recs = []
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for ln in f:
            m = line_re.match(ln.rstrip('\r\n'))
            if not m:
                continue
            p, length, ver, h, ts, hi, lo = m.groups()
            off = int(hi, 16) | (int(lo, 16) << 32)
            recs.append({'path': p.strip(), 'len': int(length), 'off': off, 'hash': h})
    return recs

import unicodedata
def sanitize(name):
    name = name.replace('\\', '/')
    bad = '<>:"/\\|?*'
    for c in bad:
        name = name.replace(c, '_')
    return name

def extract(manifest, blob, out_root, limit=None):
    recs = parse_manifest(manifest)
    recs.sort(key=lambda r: r['off'])
    total = 0
    done = 0
    with open(blob, 'rb') as bf:
        for r in recs:
            if limit and done >= limit:
                break
            rel = sanitize(r['path'])
            # drop the leading "x:/gforce/binary/" style prefix
            parts = rel.replace('\\', '/').split('/')
            rel = '/'.join(parts[3:]) if len(parts) > 3 else rel
            dest = os.path.join(out_root, rel)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            bf.seek(r['off'])
            data = bf.read(r['len'])
            if len(data) != r['len']:
                print(f"  WARN short read {rel}: got {len(data)}/{r['len']}")
            with open(dest, 'wb') as o:
                o.write(data)
            done += 1
            total += r['len']
    return done, len(recs)

if __name__ == '__main__':
    out = r"E:\G-Force\g_force\extracted\File_COM"
    n, total = extract(os.path.join(DATA, "File_COM.txt"),
                       os.path.join(DATA, "File_COM.000"), out)
    print(f"Extracted {n} files to {out}")
