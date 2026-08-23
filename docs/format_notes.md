# G-Force (2009) archive format — reverse-engineering notes

## Layout on disk (E:\G-Force\data)
- `Filelist.000`  ~1.28 GB  — main game data (MOEG container). Compressed/encoded content.
- `Filelist.bin`   29280 B   — binary index (partially parsed)
- `File_COM.000`  643 MB    — audio banks (MUSX)
- `File_COM.bin`   36864 B   — binary index (FULLY PARSED)
- `File_COM.cfi`   46907 B   — path strings (hash-named, e.g. `COM_STR_2D406A65.SFX`)
- `File_COM.txt`   66144 B   — human-readable manifest (path,Len,Ver,Hash,Loc)
- `File_RUS.000`  111 MB    — Russian audio (MUSX)
- `File_RUS.bin`  154624 B   — binary index (parsed, 1756 entries)
- Root `DATA0..DATA5` — save/profile binaries (not scripts). `EV_PC.TXT` = "1203".

## .bin binary index format (CRACKED for File_COM / File_RUS)
- Header (little-endian):
  - `+0x00` u32 version (=7)
  - `+0x04` u32 size of this .bin file
  - `+0x08` u32 entry count  (File_COM=416, File_RUS=1756; Filelist header says 410 but see below)
- Body = three INTERLEAVED parallel arrays, stride **28 bytes (0x1C)** per entry:
  - records start at file offset `0x14`
  - within each 28-byte record (offset relative to record start):
    - `+0x00` u32  length  (len of file)
    - `+0x04` u32  hash   (== hex embedded in filename, e.g. 0x2d406a65)
    - `+0x08..+0x14` u32  ver / flags / ???
    - `+0x14`(=+20) u32  offset (byte offset of file inside the .000)
    - `+0x18`(=+24) u32  length of NEXT entry (redundant)
- So: `len[k]  = u32 @ (0x14 + 28*k)`
       `hash[k] = u32 @ (0x18 + 28*k)`
       `off[k]  = u32 @ (0x28 + 28*k)`
- Self-consistency check: `sum(len) == .000 size` and `max(off+len) == .000 size` for File_COM (exact) and File_RUS (within ~6 KB — trailing padding). Files are stored contiguously.
- Path names only exist for File_COM (in .cfi / .txt). Filelist / File_RUS have NO path strings shipped, so extracted files are named by hash.

## File_RUS (count=1756) — extraction VALID
- `extract_all.py` already extracted 1756 files to `extracted/File_RUS_raw/`.

## Filelist — NOT cleanly parsed
- Header count=410, but `sum(len)=674 MB` vs blob `1.347 GB` (≈ exactly half). Offsets are ascending and `max(off+len)=1.331 GB` (within blob), so 410 files are real but they leave ~660 MB of unindexed region.
- Either Filelist.bin only indexes part of Filelist.000, or the 28-byte layout differs for this archive (no candidate reaches 100% blob coverage; confident layout is len@0x14/off@0x28 but gaps suggest per-file headers/compression).
- Content of Filelist.000 is compressed/encoded — scanning for Lua/XML/plain-text yields only garbage; long "text" runs are symbol patterns (R/[), i.e. packed binary, not scripts.

## First file of each .000
- Filelist.000 starts with magic `MOEG` (a TOC/container header?).
- File_COM.000 / File_RUS.000 start with magic `MUSX` (audio banks; contain `ESPD` chunks + PCM/ADPCM).

## Open questions / next steps
1. Fully parse Filelist.bin (find why only ~half the blob is indexed; check if there is a 2nd index or 64-bit offsets).
2. Identify the compression/encoding of Filelist.000 file contents (try zlib/deflate/LZ on extracted blobs; inspect the MOEG header).
3. Map hash -> filename for Filelist (no .cfi shipped; maybe derive from .cfi of File_COM or from strings in GForce.exe).
4. Co-op feasibility: gameplay/player logic lives in GForce.exe (x86). Making co-op requires reverse-engineering the EXE — a separate, large effort.
