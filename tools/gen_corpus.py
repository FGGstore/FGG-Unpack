#!/usr/bin/env python3
"""Builds the test corpus described in Section 8 of the requirements.

Everything is generated rather than checked in: the hostile archives are the
point of the corpus, and a repository full of traversal payloads is awkward to
move around. Run this before the extraction tests:

    python tools/gen_corpus.py

Archives land in tests/corpus/. The C tests read them by name.
"""

import os
import struct
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "tests", "corpus")

# Zip stores the "made by" host in the high byte of version_made_by; 3 is Unix,
# which is what makes the mode bits in external_attr meaningful.
UNIX_HOST = 3
S_IFLNK = 0o120000
S_IFREG = 0o100000


def _zi(name, mode=0o644, kind=S_IFREG):
    zi = zipfile.ZipInfo(name, date_time=(2024, 6, 1, 12, 0, 0))
    zi.create_system = UNIX_HOST
    zi.external_attr = (kind | mode) << 16
    zi.compress_type = zipfile.ZIP_DEFLATED
    return zi


def _symlink(zf, name, target):
    zf.writestr(_zi(name, 0o777, S_IFLNK), target)


def simple(path):
    """Nested directories, deep paths, a zero-byte file, ordinary content."""
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(_zi("readme.txt"), "hello from the corpus\n")
        zf.writestr(_zi("dir/one.txt"), "one\n")
        zf.writestr(_zi("dir/sub/two.txt"), "two\n")
        zf.writestr(_zi("dir/sub/deeper/three.bin"), bytes(range(256)) * 4)
        zf.writestr(_zi("empty.dat"), b"")
        zf.writestr(_zi("dir/sub/empty2.dat"), b"")


def unicode_names(path):
    """The console FTP server cannot handle non-ASCII; the extractor still has
    to behave predictably when an archive contains it (Section 8)."""
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(_zi("hello.txt"), "ascii\n")
        zf.writestr(_zi("café/naïve.txt"), "accents\n")
        zf.writestr(_zi("日本語/テスト.txt"), "japanese\n")
        zf.writestr(_zi("emoji-\U0001f600.txt"), "emoji\n")


def traversal(path):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(_zi("innocent.txt"), "fine\n")
        zf.writestr(_zi("../escaped.txt"), "PWNED\n")


def traversal_deep(path):
    """The bad entry is not the first: proves preflight refuses the whole job
    before writing the innocent entries that precede it (FR-2)."""
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        for i in range(20):
            zf.writestr(_zi("ok/file%02d.txt" % i), "content %d\n" % i)
        zf.writestr(_zi("a/b/../../../../../../etc/passwd"), "PWNED\n")


def absolute_path(path):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(_zi("/data/unarchiver/config.ini"), "PWNED\n")


def backslash_traversal(path):
    """A Windows-written archive can carry backslash separators; treating them
    as literal filename characters would let this through."""
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(_zi("..\\..\\escaped.txt"), "PWNED\n")


def symlink_escape(path):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(_zi("dir/ok.txt"), "fine\n")
        _symlink(zf, "dir/evil", "../../../../etc")


def symlink_safe(path):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(_zi("dir/real.txt"), "target content\n")
        _symlink(zf, "dir/link.txt", "real.txt")


def long_name(path):
    """A single component longer than UA_MAX_PATH."""
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(_zi("x" * 1200 + ".txt"), "long\n")


def near_limit_name(path):
    """A name that fits UA_MAX_PATH on its own but not once it is joined under
    a destination directory. Preflight has to catch this, otherwise the job
    starts and fails part-way through."""
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(_zi("ok.txt"), "fine\n")
        zf.writestr(_zi("n" * 1000 + ".txt"), "near the limit\n")


def deep_nesting(path):
    """More components than UA_MAX_PATH_COMPONENTS allows."""
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(_zi("/".join("d%d" % i for i in range(80)) + "/f.txt"), "deep\n")


def many_entries(path, count=20000):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        for i in range(count):
            zf.writestr(_zi("bulk/%05d.txt" % i), "%d\n" % i)


def big_single_file(path, size=48 * 1024 * 1024):
    """One entry larger than any sane read buffer, to prove FR-4 streaming:
    peak memory must not track entry size."""
    chunk = (b"0123456789abcdef" * 4096)  # 64 KiB, not very compressible in bulk
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        with zf.open(_zi("big.bin"), "w") as fh:
            written = 0
            while written < size:
                fh.write(chunk)
                written += len(chunk)


def bomb(path, size=256 * 1024 * 1024):
    """Highly compressible payload: passes a naive free-space check but expands
    far beyond its own size. Guarded by max_expansion_ratio."""
    chunk = b"\0" * (1024 * 1024)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        with zf.open(_zi("zeros.bin"), "w") as fh:
            for _ in range(size // len(chunk)):
                fh.write(chunk)


def truncated(path, source):
    """Cutting the tail removes the central directory, which is where a zip
    reader starts."""
    with open(source, "rb") as fh:
        data = fh.read()
    with open(path, "wb") as fh:
        fh.write(data[: int(len(data) * 0.6)])


def corrupt_body(path, source):
    """Keeps the central directory intact but scrambles compressed data, so the
    failure happens mid-entry rather than at open (FR-6)."""
    with open(source, "rb") as fh:
        data = bytearray(fh.read())
    for i in range(40, min(len(data) - 40, 400)):
        data[i] ^= 0xFF
    with open(path, "wb") as fh:
        fh.write(bytes(data))


def not_an_archive(path):
    with open(path, "wb") as fh:
        fh.write(b"this is plain text, not an archive at all\n" * 20)


def empty_file(path):
    open(path, "wb").close()


def plain_tar(path):
    """Minimal ustar archive: detection must recognise it by the magic at
    offset 257 even though this build cannot extract it yet."""
    name = b"hello.txt"
    body = b"tar content\n"

    hdr = bytearray(b"\0" * 512)
    hdr[0 : len(name)] = name
    hdr[100:108] = b"000644 \0"
    hdr[108:116] = b"000000 \0"
    hdr[116:124] = b"000000 \0"
    hdr[124:136] = ("%011o " % len(body)).encode()
    hdr[136:148] = b"14000000000 "
    hdr[148:156] = b" " * 8  # checksum field is spaces while summing
    hdr[156:157] = b"0"
    hdr[257:265] = b"ustar\x0000"

    chksum = sum(hdr) & 0o7777777
    hdr[148:156] = ("%06o\0 " % chksum).encode()

    with open(path, "wb") as fh:
        fh.write(bytes(hdr))
        fh.write(body)
        fh.write(b"\0" * (512 - len(body)))
        fh.write(b"\0" * 1024)  # two empty blocks terminate the archive


def gzip_stub(path):
    with open(path, "wb") as fh:
        fh.write(b"\x1f\x8b\x08\x00" + struct.pack("<I", 0) + b"\x00\x03")
        fh.write(b"\x03\x00" + struct.pack("<II", 0, 0))


def xz_stub(path):
    with open(path, "wb") as fh:
        fh.write(b"\xfd7zXZ\x00" + b"\x00" * 32)


def sevenz_stub(path):
    with open(path, "wb") as fh:
        fh.write(b"7z\xbc\xaf\x27\x1c" + b"\x00" * 32)


# --- 7z ------------------------------------------------------------------
#
# Generated with the 7-Zip CLI rather than written by hand: the point is to
# decode archives a real 7-Zip produced, including its default LZMA2 settings
# and solid blocking. When 7z is not installed the 7z corpus is skipped and the
# C tests that need it skip too, so the rest of the suite still runs.

SEVENZ_CANDIDATES = [
    r"C:\Program Files\7-Zip\7z.exe",
    r"C:\Program Files (x86)\7-Zip\7z.exe",
    "7z",
    "7zz",
    "7za",
]


def find_7z():
    import shutil

    for c in SEVENZ_CANDIDATES:
        if os.path.isabs(c):
            if os.path.isfile(c):
                return c
        else:
            found = shutil.which(c)
            if found:
                return found
    return None


def run_7z(exe, args):
    import subprocess

    r = subprocess.run([exe] + args, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    if r.returncode != 0:
        raise RuntimeError("7z failed: " + r.stdout.decode("utf-8", "replace"))


def build_7z_corpus(exe, tmp):
    """Mirrors the zip cases that matter for the 7z decode path."""
    import shutil

    made = []

    def stage(name):
        d = os.path.join(tmp, name)
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        return d

    def archive(name, srcdir, extra=None):
        path = os.path.join(OUT, name)
        if os.path.exists(path):
            os.remove(path)
        args = ["a", "-t7z", "-bso0", "-bsp0", path,
                os.path.join(srcdir, "*")]
        run_7z(exe, args + (extra or []))
        made.append(name)

    # Plain LZMA2, the default and by far the most common.
    d = stage("simple")
    with open(os.path.join(d, "readme.txt"), "wb") as f:
        f.write(b"hello from the 7z corpus\n")
    os.makedirs(os.path.join(d, "dir", "sub"))
    with open(os.path.join(d, "dir", "one.txt"), "wb") as f:
        f.write(b"one\n")
    with open(os.path.join(d, "dir", "sub", "two.txt"), "wb") as f:
        f.write(b"two\n")
    with open(os.path.join(d, "empty.dat"), "wb") as f:
        pass
    archive("simple.7z", d)

    # Solid: several files share one compressed block, so reaching the third
    # means decoding and discarding the first two. That is the path a naive
    # implementation gets wrong.
    d = stage("solid")
    for i in range(8):
        with open(os.path.join(d, "part%02d.bin" % i), "wb") as f:
            f.write(bytes([i]) * 200000)
    archive("solid.7z", d, ["-ms=on"])

    # Stored, no compression: exercises the copy coder.
    d = stage("stored")
    with open(os.path.join(d, "stored.txt"), "wb") as f:
        f.write(b"no compression here\n" * 100)
    archive("stored.7z", d, ["-m0=Copy"])

    # LZMA1 rather than LZMA2.
    d = stage("lzma1")
    with open(os.path.join(d, "lzma1.bin"), "wb") as f:
        f.write((b"repeating pattern " * 5000))
    archive("lzma1.7z", d, ["-m0=LZMA"])

    # Large enough that it cannot be a single decode call, proving the stream
    # loop rather than a lucky one-shot.
    d = stage("big")
    with open(os.path.join(d, "big.bin"), "wb") as f:
        chunk = bytes(range(256)) * 256          # 64 KiB, mildly compressible
        for _ in range(12 * 16):                 # 12 MiB
            f.write(chunk)
    archive("big.7z", d)

    # Same dictionary size a large real-world archive uses (LZMA2:28 = 256 MB).
    # The payload has to allocate that window on the console, so it is worth
    # proving the path works rather than discovering it on hardware.
    d = stage("bigdict")
    with open(os.path.join(d, "bigdict.bin"), "wb") as f:
        f.write(b"dictionary window test " * 40000)
    archive("bigdict.7z", d, ["-m0=LZMA2:d256m"])

    # Enough data, a small dictionary and multi-threaded compression, so
    # 7-Zip emits many LZMA2 blocks -- each one a dictionary reset, which is
    # the only place the stream can be split for parallel decoding.
    #
    # Content is position-derived: every 64 KiB run holds a distinct byte. An
    # offset mistake in the parallel decoder therefore produces wrong content
    # rather than merely the wrong length, which is what a size check alone
    # would miss.
    d = stage("parallel")
    with open(os.path.join(d, "parallel.bin"), "wb") as f:
        for i in range(768):                      # 768 * 64 KiB = 48 MiB
            f.write(bytes([i & 0xFF]) * 65536)
    archive("parallel.7z", d, ["-m0=LZMA2:d1m", "-mmt=4"])

    # A BCJ2 filter chain, which this build deliberately refuses rather than
    # mis-decodes. 7-Zip picks BCJ2 for x86 executables when asked.
    d = stage("bcj2")
    with open(os.path.join(d, "code.bin"), "wb") as f:
        f.write((b"\x55\x8b\xec\xe8\x10\x00\x00\x00\x5d\xc3" * 20000))
    archive("bcj2.7z", d, ["-m0=BCJ2", "-m1=LZMA2", "-m2=LZMA", "-m3=LZMA"])

    return made


def build_7z_hostile(exe, tmp, out_name="traversal.7z"):
    """A 7z whose entry name escapes the destination.

    7-Zip will not write "../" into an archive, so the name is patched
    afterwards. That needs the header uncompressed (-mhc=off) to find the name,
    and both header CRCs recomputed, or the SDK rejects the file as damaged
    before path safety is ever consulted -- which would make the test pass for
    entirely the wrong reason.

    The 32-byte signature header is:
      0..5   magic
      6,7    version
      8..11  CRC32 of bytes 12..31
      12..19 NextHeaderOffset (relative to byte 32)
      20..27 NextHeaderSize
      28..31 CRC32 of the next header
    """
    import shutil
    import struct
    import zlib

    src_dir = os.path.join(tmp, "hostile")
    shutil.rmtree(src_dir, ignore_errors=True)
    os.makedirs(src_dir)

    # Same length as "../evil.txt" so the patch is byte-for-byte in place.
    placeholder = "AAAevil.txt"
    target = "../evil.txt"
    assert len(placeholder) == len(target)

    with open(os.path.join(src_dir, placeholder), "wb") as f:
        f.write(b"PWNED\n")

    path = os.path.join(OUT, out_name)
    if os.path.exists(path):
        os.remove(path)
    run_7z(exe, ["a", "-t7z", "-mhc=off", "-bso0", "-bsp0", path,
                 os.path.join(src_dir, "*")])

    with open(path, "rb") as f:
        data = bytearray(f.read())

    old = placeholder.encode("utf-16-le")
    if old not in data:
        print("  (could not patch 7z name; skipping traversal.7z)")
        os.remove(path)
        return None

    i = data.index(old)
    data[i:i + len(old)] = target.encode("utf-16-le")

    # Repair the two CRCs the SDK checks before it looks at any name.
    next_off, next_size = struct.unpack_from("<QQ", data, 12)
    start = 32 + next_off
    header = bytes(data[start:start + next_size])
    struct.pack_into("<I", data, 28, zlib.crc32(header) & 0xFFFFFFFF)
    struct.pack_into("<I", data, 8,
                     zlib.crc32(bytes(data[12:32])) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(bytes(data))

    return out_name


def main():
    os.makedirs(OUT, exist_ok=True)

    def p(name):
        return os.path.join(OUT, name)

    simple(p("simple.zip"))
    unicode_names(p("unicode.zip"))
    traversal(p("traversal.zip"))
    traversal_deep(p("traversal_deep.zip"))
    absolute_path(p("absolute.zip"))
    backslash_traversal(p("backslash.zip"))
    symlink_escape(p("symlink_escape.zip"))
    symlink_safe(p("symlink_safe.zip"))
    long_name(p("longname.zip"))
    near_limit_name(p("nearlimit.zip"))
    deep_nesting(p("deep.zip"))
    many_entries(p("many.zip"))
    big_single_file(p("big.zip"))
    bomb(p("bomb.zip"))
    truncated(p("truncated.zip"), p("simple.zip"))
    corrupt_body(p("corrupt.zip"), p("simple.zip"))
    not_an_archive(p("notarchive.bin"))
    empty_file(p("empty.bin"))
    plain_tar(p("plain.tar"))
    gzip_stub(p("stub.gz"))
    xz_stub(p("stub.xz"))
    sevenz_stub(p("stub.7z"))

    exe = find_7z()
    if exe:
        import tempfile
        import shutil

        tmp = tempfile.mkdtemp(prefix="ua7z")
        try:
            build_7z_corpus(exe, tmp)
            build_7z_hostile(exe, tmp)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    else:
        print("  (7z not found: skipping the 7z corpus)")

    total = 0
    for name in sorted(os.listdir(OUT)):
        size = os.path.getsize(os.path.join(OUT, name))
        total += size
        print("  %-22s %10d" % (name, size))
    print("%d files, %.1f MB" % (len(os.listdir(OUT)), total / 1048576.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
