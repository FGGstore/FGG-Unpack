# Third-party code

Both libraries are vendored in `third_party/` rather than fetched at build
time. The PS5 SDK toolchain has no package manager, and pinning the exact
sources that were tested is worth more here than tracking upstream.

Both are compatible with this project's GPL-3 licence. That was a selection
criterion, not a coincidence: `.rar` support is excluded from the project
specifically because the reference unrar library's terms conflict with GPL
distribution, so a decoder that raised the same problem would have been no
better.

---

## miniz

- **Location:** `third_party/miniz/`
- **Version:** 11.3.2
- **Upstream:** https://github.com/richgel999/miniz
- **Licence:** MIT (see `third_party/miniz/LICENSE`)
- **Used for:** zip reading, and the inflate implementation

Unmodified except for `miniz_export.h`, which upstream generates with CMake.
This project does not use CMake, so that header is written by hand and defines
`MINIZ_EXPORT` empty for a static build.

## LZMA SDK

- **Location:** `third_party/lzma/`
- **Version:** 23.01
- **Upstream:** https://www.7-zip.org/sdk.html
- **Licence:** public domain (see `third_party/lzma/LICENSE.txt`)
- **Used for:** 7z parsing, and the LZMA/LZMA2 decoders

Unmodified. Only the subset needed to *read* archives is vendored — the
encoders, PPMd and the multi-threaded decoder are all left out.

Two functions the SDK declares in `7z.h` but defines nowhere in 23.01,
`SzArEx_GetFolderStreamPos` and `SzArEx_GetFolderFullPackSize`, are implemented
in `src/backend/ua_be_7z.c` instead of by patching the vendored tree, so
`third_party/lzma/` stays byte-identical to the upstream release.

---

## What is not used

`SzArEx_Extract()`, the SDK's own extraction entry point, decodes an entire
solid block into a single allocation. A 7z holding one 150 GB disk image is one
solid block, so calling it would attempt a 150 GB `malloc`. This project drives
`Lzma2Dec`/`LzmaDec` directly and streams instead, which is also what FR-4 in
the requirements demands.
