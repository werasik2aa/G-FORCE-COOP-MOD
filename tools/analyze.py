import os, re, sys
from collections import Counter

ROOT = r"E:\G-Force\data"
WORK = r"E:\G-Force\g_force"

# Real format (no leading index; Read tool added line numbers):
# x:\gforce\binary\...\<file>   : Len <len> : Ver <ver> : Hash <hash> : Ts <ts> :  Loc <loc_hi>:<loc_lo>
line_re = re.compile(
    r'^\s*(.+?)\s*:\s*Len\s+(\d+)\s*:\s*Ver\s+(\d+)\s*:\s*Hash\s+(0x[0-9a-fA-F]+)'
    r'\s*:\s*Ts\s+(0x[0-9a-fA-F]+)\s*:\s*Loc\s+([0-9a-fA-F]+):([0-9a-fA-F]+)\s*$'
)

def parse_manifest(path):
    recs = []
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for ln in f:
            m = line_re.match(ln.rstrip('\n').rstrip('\r'))
            if not m:
                continue
            p, length, ver, h, ts, loc_hi, loc_lo = m.groups()
            off = int(loc_hi, 16) | (int(loc_lo, 16) << 32)
            recs.append({
                'path': p.strip(),
                'len': int(length),
                'ver': int(ver),
                'hash': h,
                'off': off,
            })
    return recs

def ext_hist(recs):
    c = Counter()
    for r in recs:
        base = os.path.basename(r['path'].replace('\\', '/'))
        e = os.path.splitext(base)[1].lower()
        c[e] += 1
    return c

if __name__ == '__main__':
    manifests = [f for f in os.listdir(ROOT) if f.endswith('.txt')]
    for mf in sorted(manifests):
        p = os.path.join(ROOT, mf)
        recs = parse_manifest(p)
        blob = os.path.join(ROOT, mf[:-4] + '.000')
        total = sum(r['len'] for r in recs)
        print(f"\n=== {mf} : {len(recs)} entries, blob exists={os.path.exists(blob)}")
        print(f"    sum(len)={total:,}  blob size={os.path.getsize(blob) if os.path.exists(blob) else 'NA'}")
        h = ext_hist(recs)
        for ext, n in h.most_common(40):
            print(f"  {ext or '(none)':10} {n}")
