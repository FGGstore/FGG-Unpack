# FGG Unpack

**Extract `.zip` and `.7z` archives directly on a jailbroken PlayStation 5.**

[![CI](https://github.com/FGGstore/FGG-Unpack/actions/workflows/ci.yml/badge.svg)](https://github.com/FGGstore/FGG-Unpack/actions/workflows/ci.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

A jailbroken PS5 has no way to unpack an archive. Anything that arrives
compressed — homebrew bundles, asset packs, emulator data, disk images — has to
be extracted on a PC first and then copied over FTP file by file.

That is slow, and it is slow in the worst possible way: you end up transferring
the *uncompressed* size.

FGG Unpack runs in the background on the console, watches a folder, and unpacks
whatever you drop into it.

```
             upload            extract
  30 GB  ──────────────►  PS5  ────────►  150 GB
   .7z          FTP                        image

  instead of transferring 150 GB
```

Measured on real hardware: a **31 GB** archive containing a **152 GB** image.
Uploading it took ~40 minutes. Uploading the extracted image would have taken
over **three hours** at the same link speed.

---

## Install

Grab `fgg-unpack.elf` from the [latest release](https://github.com/FGGstore/FGG-Unpack/releases/latest).

**With Payload Manager** — copy `fgg-unpack.elf` and `fgg-unpack.elf.json` over
FTP into:

```
/data/pldmgr/payloads/fgg-unpack/
```

then launch **FGG Unpack** from the Payload Manager web page.

**Without it** — send `fgg-unpack.elf` to port **9021** using any payload
sender.

**On every boot** — add `fgg-unpack.elf` to `/data/pldmgr/autoload.txt`.

## Use it

1. Drop a `.zip` or `.7z` into `/data/homebrew` over FTP
2. Wait

That is the whole workflow. The archive is detected, checked, and unpacked into
the folder it came from. You get a notification every 5%:

```
FGG Unpack: PPSA21564.7z  45%  73.8 GB / 152.7 GB  42 MB/s  36m left
```

The same detail is written to `/data/fgg-unpack/debug.log`, which survives to
be read afterwards if something goes wrong.

---

## What it does carefully

Extraction runs unattended on a console you cannot easily debug, so the
defaults lean towards refusing rather than guessing.

**It checks before it starts.** Entry count, uncompressed size, and free space
are all worked out first. If the job cannot fit, it is refused before a single
byte is written — you get a clear reason instead of a half-extracted mess and a
full disk.

**It refuses hostile archives.** Every entry path is validated before anything
is created: absolute paths, `../` traversal (Zip Slip), symlinks pointing
outside the destination, embedded null bytes, over-long names. One bad entry
rejects the whole archive, even if it is the last one.

**It cannot fill your disk.** A small archive claiming to expand to hundreds of
gigabytes is refused. Logs rotate at a size cap. The job history is a fixed-size
ring.

**Memory stays flat.** A 150 GB entry is streamed, never buffered. Peak memory
is the decompression window plus a few hundred KB, regardless of archive size.

**It waits for uploads to finish.** An archive is ignored until its size *and*
timestamp have stopped changing, so a file still arriving over FTP is never
grabbed half-written.

**It never deletes anything by surprise.** `delete_after_extract` is off by
default, and even when enabled the archive is only removed after the entry
count, the file count and every file size have been verified against the
archive's own metadata.

**Only one copy runs at a time.** Launching twice is easy to do by accident;
the second copy exits instead of racing the first.

---

## Configuration

`/data/fgg-unpack/config.ini`, written with commented defaults on first run.
Every key is optional, and an unknown key or bad value is logged and skipped
rather than preventing startup.

| Key | Default | Meaning |
|---|---|---|
| `watchpath` | `/data/homebrew` | Directory to scan. Repeat for more than one. |
| `scan_depth` | `1` | `1` = top level only, `2` = one level down |
| `poll_interval_seconds` | `10` | How often to look |
| `stability_wait_seconds` | `15` | Quiet period before touching a file |
| `overwrite` | `0` | Replace existing files at the destination |
| `delete_after_extract` | `0` | Remove the archive after a *verified* extraction |
| `min_free_margin_mb` | `1024` | Free space required beyond the payload |
| `chunk_size_kb` | `256` | Read/write buffer size |
| `decode_threads` | `0` | 7z decode workers; `0` = auto, `1` = off |
| `decode_memory_budget_mb` | `1024` | Ceiling on workers × dictionary |
| `progress_percent_step` | `5` | Report every N percent |
| `notify` | `1` | System notifications |
| `debug` | `1` | Verbose logging |

For archives outside the watched folders, list them one per line in
`/data/fgg-unpack/queue.txt`:

```
/mnt/usb0/downloads/pack.7z
/data/backups/configs.zip -> /data/restored/
```

## Supported formats

| Format | Status |
|---|---|
| `.zip` | Yes — deflate and stored |
| `.7z` | Yes — LZMA, LZMA2 and stored, solid or not |
| `.tar`, `.tar.gz`, `.tar.xz` | Detected, refused with a clear reason |
| `.rar` | Out of scope — no cleanly licensed decoder |

Format is decided by **content, not file extension**. Renaming a `.zip` to
`.bin` changes nothing; so does renaming a text file to `.zip`.

7z archives using BCJ2 filter chains or encryption are refused by name rather
than mis-decoded.

---

## Speed

7z decoding runs in parallel. An LZMA2 stream can only be split where it resets
its dictionary — a real 31 GB archive has around 600 such points — so workers
each take a range and write at their own offset.

Measured on console with a 152 GB image:

| | throughput | total |
|---|---|---|
| single-threaded | 30 MB/s | 87 min |
| parallel | 47 MB/s | ~57 min |

The parallel figure sits at the console's sustained write speed, so the
bottleneck is now storage rather than decompression. `decode_threads=1` turns
it off if you would rather have the simpler path.

> While extracting, the output file's **size is not progress**. Workers write at
> different offsets, so an FTP client shows the file at nearly its final size
> almost immediately and then apparently stalled. The log is the real measure.

---

## Building

Nothing is fetched at build time — miniz and the LZMA SDK are vendored in
`third_party/`.

**The payload** needs Linux or WSL and the
[PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk):

```sh
sudo apt install clang lld llvm make unzip     # llvm is required, and easy to miss
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make -f Makefile.ps5
```

**The tests** need Visual Studio and Python 3:

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_tests.ps1
```

That generates the test corpus, builds, and runs 366 checks.

## Checking an archive from your PC

The same build produces `fgg-unpack.exe` — the extraction core with a command
line on it. Use it to find out whether an archive will work *before* spending
half an hour uploading it.

```
fgg-unpack <archive>                    inspect only, writes nothing
fgg-unpack --extract <archive> [dest]   actually unpack
```

```
$ fgg-unpack PPSA21564.7z
format:      7z
  archive size           29.17 GB
  entries                1
  uncompressed           152.68 GB
  expansion              5.23x
  required               153.68 GB
preflight passed
```

It refuses exactly what the console refuses, so a "no" here is a "no" there.

---

## How it is built

The extraction core is portable C and knows nothing about the PlayStation.
`src/platform/ua_platform.h` is the entire operating-system surface;
`src/backend/ua_backend.h` is the entire archive-format surface. Everything
else sits above both.

That is what makes the test suite meaningful: the same code that ships in the
payload is compiled for a PC and run against a generated corpus of nested
paths, non-ASCII names, zero-byte and multi-gigabyte files, truncated and
corrupted archives, 20,000-entry archives, and deliberately malicious ones.
Only platform integration needs real hardware.

```
src/
  core/      path safety, preflight, streaming extraction, watch driver
  backend/   content detection, zip via miniz, 7z via the LZMA SDK
  platform/  files, directories, threads, notifications
tests/       366 checks
tools/       corpus generator and build scripts
docs/        design notes and SDK findings
```

`docs/` is worth reading if you are curious why things are the way they are —
including the bugs found along the way and the requirements the design does not
yet meet.

## Licence

[GPL-3.0](LICENSE). Vendored dependencies and their licences are listed in
[THIRD-PARTY.md](THIRD-PARTY.md); all are GPL-compatible.

## Credits

Built by **FGG STORE**.

Standing on the work of [ps5-payload-dev](https://github.com/ps5-payload-dev)
for the SDK, `shsrv` and `ftpsrv`; [miniz](https://github.com/richgel999/miniz)
and the [LZMA SDK](https://www.7-zip.org/sdk.html) for the decoders; and
[ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) for the
watch-folder patterns this follows.

> Not affiliated with Sony Interactive Entertainment. For use with homebrew on
> consoles you own.
