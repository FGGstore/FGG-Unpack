# PS5 Payload SDK — verified facts

SDK **v0.43** (released 2026-08-29) is installed at `/opt/ps5-payload-sdk` in
WSL Ubuntu. `samples/hello_world` and `unarchiver.elf` both build.

Everything below marked *verified* was confirmed by running it. Anything about
console behaviour is still unproven: linking only shows a symbol exists in the
SDK stubs, not that it works on hardware.

## Setup that actually worked

```sh
# In WSL Ubuntu 24.04, as root.
apt-get install -y --no-install-recommends clang lld make unzip
apt-get install -y --no-install-recommends llvm-18      # see below
unzip -d /opt ps5-payload-sdk.zip
chmod -R +x /opt/ps5-payload-sdk/bin
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
```

Release asset:
`https://github.com/ps5-payload-dev/sdk/releases/download/v0.43/ps5-payload-sdk.zip`
(8.4 MB). It unpacks to `ps5-payload-sdk/`, so `-d /opt` gives exactly the
`/opt/ps5-payload-sdk` the SDK expects.

**The undocumented dependency:** the SDK README lists `clang-18 lld-18`, but
`llvm-config` is also required and ships in the separate `llvm-18` package.
Without it the failure is opaque — `bin/prospero-llvm-config` finds no
candidate, `LLVM_BINDIR` comes out empty, and the build dies with:

```
/opt/ps5-payload-sdk/bin/clang: line 45: /llvm-clang: No such file or directory
```

## Windows

The zip contains a `win/` directory with `prospero-clang.cmd`,
`prospero-deploy.cmd`, a bundled `prospero-lld.exe`, `ninja.exe` and
`plink.exe`. So Windows is supported — but only through CMake
(`prospero-cmake.cmd`, `toolchain/prospero.cmake`); there is no `.mk` wrapper.
The `.cmd` also invokes a bare `clang`, so a Windows LLVM install is still
needed. Building here would mean writing a `CMakeLists.txt` and leaving
`Makefile.ps5` unused, which is why WSL was chosen.

## Install and toolchain

| Thing | Value |
|---|---|
| Install prefix | `/opt/ps5-payload-sdk` (`sudo unzip -d /opt ps5-payload-sdk.zip`) |
| Environment variable | `PS5_PAYLOAD_SDK` |
| Host requirements | clang and lld (the SDK probes `llvm-config-15` through `-22`) |
| Project include | `$(PS5_PAYLOAD_SDK)/toolchain/prospero.mk` |
| Host OS | Linux or macOS. Not Windows — use WSL. |

`toolchain/prospero.mk` is **generated at install time**. Its source lives at
`host/toolchain/prospero.mk` in the repo, which is why the path a project
includes does not exist in the SDK source tree. Chasing that discrepancy is
worth avoiding twice.

`Makefile.inc` in the repo root is the SDK's own build configuration. Projects
do not include it.

### What prospero.mk defines

```make
PS5_PAYLOAD_SDK := ...
PS5_SYSROOT     := $(PS5_PAYLOAD_SDK)/target
PS5_HBROOT      := /user/homebrew
PS5_DEPLOY := $(PS5_PAYLOAD_SDK)/bin/prospero-deploy
CC         := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
CXX        := $(PS5_PAYLOAD_SDK)/bin/prospero-clang++
LD         := $(PS5_PAYLOAD_SDK)/bin/prospero-lld
AR         := $(PS5_PAYLOAD_SDK)/bin/prospero-ar
STRIP      := $(PS5_PAYLOAD_SDK)/bin/prospero-strip
```

Plus `AS`, `NM`, `OBJCOPY`, `RANLIB`, `PKG_CONFIG`, all `prospero-` prefixed.

### Deploying

`samples/hello_world/Makefile` uses `PS5_HOST ?= ps5` and `PS5_PORT ?= 9021`,
then `$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $(ELF)`. Port 9021 matches
the requirements. There is also a gdb stub on **port 2159**, which the sample
drives with `gdb-multiarch` — worth knowing given how much section 9 stresses
that the target is hard to debug. Both targets are now in `Makefile.ps5`.

---

## Two bugs the first console build found

Neither was visible to the host harness. This is the argument for doing M1
early rather than at the end.

**Trigraphs.** `ua_log.c` had `strcpy(stamp, "????-??-?? ??:??:??")` as the
fallback when `strftime` fails. Under strict C11, `??-` is a trigraph for `~`,
so clang converted it and `-Werror` stopped the build. MSVC never mentioned it.
The fallback timestamp would have come out mangled. Now `0000-00-00 00:00:00`.

**miniz zlib aliases.** `miniz.h` defines static inline `crc32`, `inflate`,
`uncompress` and a dozen more in every translation unit that includes it. We
only call the `mz_*` names, so under `-Wall -Wextra -Werror` every one is an
unused-function error. Fixed with `-DMINIZ_NO_ZLIB_COMPATIBLE_NAMES` on our own
sources only — miniz's own translation units may use those aliases internally.
The same define is now in `tools/build_host.ps1`, so both builds compile
identical code.

**What linked.** `unarchiver.elf` is 246 KB, a 64-bit x86-64 PIE. Its undefined
symbols are `statvfs`, `sceKernelSendNotificationRequest`, the POSIX file calls
(`open`, `lstat`, `mkdir`, `rename`, `nanosleep`) and ordinary libc — all
resolved as dynamic imports against the SDK stubs, which is exactly what a
payload should look like.

miniz cross-compiled without a single patch. The requirements expected the
extraction library to be "the single largest task in Milestone 1"; choosing
miniz over libarchive reduced it to nothing.

---

## Open question 1 — free space: **mostly answered**

`statvfs` and `fstatvfs` are declared in
`include/freebsd/sys/statvfs.h` and exported by **libSceLibcInternal**, so
`ua_plat_posix.c` should link as written. They are not implemented in the SDK's
own libc (81 source files, none for statvfs), so they come from the console.

The alternative, if that ever fails to link, is `statfs` / `fstatfs` /
`getfsstat` from **libkernel_sys** via `<sys/mount.h>`, using
`f_bavail * f_bsize`. That is the route the SDK's own `samples/mntinfo` takes,
with `getmntinfo(&buf, MNT_WAIT)`.

**Still needs hardware:** whether either reports correctly for `/mnt/usb*`.
`samples/mntinfo` is the ready-made way to check — build and run it, and it
prints every mount.

---

## Open question 2 — notifications: **answered, and it found a bug**

The entry point is `sceKernelSendNotificationRequest`, exported by both
`libkernel` and `libkernel_sys`. There is **no SDK header for it**, so callers
declare it themselves. `ps5-payload-dev/shsrv` — the long-running-payload
reference the requirements already cite — declares it as:

```c
typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);
```

and calls it as `sceKernelSendNotificationRequest(0, &req, sizeof req, 0)`
after zeroing the struct and formatting into `req.message`.

**The bug this found:** `ua_plat_posix.c` had declared it taking a bare
`const char *` and passed `strlen(msg) + 1` as the length. The kernel reads the
full 3120-byte structure regardless of the length argument, so that would have
read far past the end of a short stack buffer on every notification — on a
payload whose first non-functional requirement is "a crash can destabilise the
system". Now fixed to match shsrv, with `snprintf` bounding the message so a
long failure reason truncates instead of overflowing.

There is a second, newer API — `sceNotificationSend(int userId, bool isLogged,
const char *payload)` in **libSceNotification**, taking a JSON payload
(`samples/notify` shows an `InteractiveToastTemplateB` template with icon,
title and progress fields). It is richer and could drive a real progress toast
later. `samples/notify_debug` exists too. The shsrv route is the simpler one
and is what M1 should confirm first.

**Still needs hardware:** whether there is a rate limit. The requirement to
rate-limit progress notifications stands regardless, and
`progress_percent_step` already does it.

---

## Not yet investigated

- **Open question 3** — whether libarchive cross-compiles. Moot for now: the
  build uses miniz, which v2 explicitly endorses for a zip-only v1.
- **Open question 4** — whether port 9022 is free for the web UI. The SDK
  itself uses 9021 for payloads and 2159 for gdb, so neither collides.
  `ps5-payload-dev/websrv` is the reference to check for what it binds.
- **Open question 5** — whether a community payload already does this. Still
  unchecked, still the cheapest question on the list.

---

## What is on this machine

No SDK, and no compiler that could host one. Present in `~/Downloads`:
NetCat GUI v1.3 (the payload sender the requirements name), `kstuff.elf`,
`util.elf`, `etaHEN-2.5B.bin`, `OnionHEN.elf`, and Y2JB jailbreak bundles for
firmware 12.40 and 7.61. `D:\PS5` exists but is empty.

To start M1, in WSL Ubuntu:

```sh
sudo apt install clang lld make python3      # WSL has none of these yet
# fetch and unzip the SDK release to /opt/ps5-payload-sdk
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make -C $PS5_PAYLOAD_SDK/samples/hello_world
```

Getting `samples/hello_world` and `samples/notify` to run on the console is
Milestone 1 in its entirety, and `samples/mntinfo` answers open question 1 at
the same time.
