# Gaps found in the requirements

Written while implementing the v1 draft, then re-checked against
`Ps5UnarchiverRequirementsV2.md`. Each item is something the requirements
leave undefined or under-specified, what the code does about it now, and
whether the spec still needs a decision from you.

**v2 closed three of these on its own** — items 4, 5 and 9 below are marked
accordingly. Requirement numbers cited are v2 numbers.

---

## 1. A zip bomb passes the FR-2 free-space check

**Gap.** FR-2 step 4 checks free space against the *declared* uncompressed
size. A 260 KB archive declaring 256 MB passes that check comfortably on a
console with a spare gigabyte, and then fills the disk.

**Now.** `max_expansion_ratio` (default 200) refuses an archive whose declared
total exceeds that multiple of its own file size, and
`expansion_floor_bytes` (default 64 MB) exempts small archives, which
legitimately compress enormously and are harmless. Setting the ratio to `0`
disables the guard. `tests/test_extract.c:test_bomb_guard` covers both
directions.

**Still needed.** Whether 200x is right for the content you actually extract.
A tar of text config files can exceed it.

---

## 2. The declared size is not enforced during extraction

**Gap.** Related but separate: even with a sane ratio, nothing stopped a
stream from writing more bytes than its own header declared.

**Now.** `file_sink_write` aborts the entry the moment writes exceed the
declared size, and `extract_one_file` fails the job if an entry finishes
*short* of it. Both report `UA_ERR_CORRUPT`.

---

## 3. No cleanup policy after a mid-extraction failure

**Gap.** FR-3 says abort the whole job on the first path violation. FR-7 says
do not silently leave a half-extracted tree without recording it. Neither says
whether the partial output should be *deleted*.

**Now.** Partial output is deliberately left in place and the failure recorded.
Deleting it would destroy whatever was already at the destination when
`overwrite=1`, which is worse than leaving a partial tree the log names.

**Still needed.** A decision. If you want cleanup, it should be opt-in
(`cleanup_on_failure`) and must only remove files this job actually created.

---

## 4. `stability_wait_seconds` uses mtime alone — **closed by v2**

**Gap.** The v1 draft defined the setting as "minimum age of an archive
before processing". Some FTP servers set the final mtime at the start of a
transfer, so an archive still arriving can look old enough.

**v2 4.1** now requires that "size and mtime have been stable", which is the
right rule, and raises the default to 15 seconds.

**Now, implemented in M3.** `ua_driver.c` keeps a candidate table holding
`(size, mtime, stable_since)` for every file it has seen. A job is offered
to the extractor only once both values have been observed unchanged for
`stability_wait_seconds`, so a single sighting is never enough no matter
how old the mtime claims to be. `test_growing_file_resets_the_clock`
appends to a file mid-wait and asserts the clock restarts.

`ua_extract_run` keeps its own age check as a backstop for callers that
are not the driver; the driver disables it, having already done better.

---

## 5. `queue.status` needs atomic writes — **partly closed by v2**

**Gap.** Section 7 requires idempotent startup and an uncorrupted
`queue.status`. Appending to it in place makes a crash mid-write able to leave
a torn final record.

**v2 4.3** settles the keying question — path + size + mtime, so replacing a
file with a new version of the same name correctly re-triggers. It still does
not say how the file is written.

**Now, implemented in M3.** `ua_status.c` writes `queue.status.tmp` and then
calls `ua_plat_replace()`, which is `rename()` on the console and
`MoveFileExW(MOVEFILE_REPLACE_EXISTING)` on the host — plain `rename()`
fails on Windows when the destination exists, so the harness would
otherwise have tested something the console does not do.

The record set is a fixed ring of `UA_STATUS_MAX` entries, so the file is
bounded as section 7 requires, and a damaged line is skipped rather than
refusing to start.

---

## 6. No bound on entry count or path depth

**Gap.** The spec bounds the log size and free space but nothing about the
archive's own shape. An archive with a million entries or a path 5000
components deep is a denial of service on a console that has no way to kill a
stuck payload short of a reboot.

**Now.** `UA_MAX_PATH` (1024, matching FreeBSD `PATH_MAX`),
`UA_MAX_PATH_COMPONENTS` (64) and `max_entries` (2,000,000). All refuse during
preflight, so nothing is written.

---

## 7. Symlinks: FR-3 is necessary but not sufficient

**Gap.** FR-3 asks that symlinks pointing *outside* the destination be
rejected. A symlink pointing *inside* it is still a way to redirect entries
extracted later in the same archive — extract `link -> subdir`, then extract
`link/x`, and the write lands wherever `subdir` resolves to.

**Now.** Three layers:

- `allow_symlinks` defaults to `0`; links are skipped and counted, not written.
- When enabled, the FR-3 containment check runs (`ua_path_check_symlink`),
  and it checks depth at *every* component, so `../x/../../..` is caught even
  though its running total returns to zero mid-walk.
- Independently of both, files are opened with `O_NOFOLLOW` and directories are
  refused if any existing component is a symlink, so a planted link cannot
  redirect a write even if it somehow got created.

**Still needed.** Confirmation that nothing you extract actually needs
symlinks. If something does, `allow_symlinks=1` is the switch.

---

## 8. Free-space check runs before the destination exists

**Gap.** FR-2 orders the free-space check (step 4) before creating the
destination directory (step 5), but `statvfs` needs a path that exists.

**Now.** `free_space_for()` walks up to the nearest existing ancestor. The
ordering in the spec is the right one — nothing should be created until every
check passes — so the implementation keeps it.

---

## 9. Notification rate limiting was specified but not quantified

**Gap.** v2 4.6 says progress notifications should be rate-limited, "one per
N percent", without fixing N.

**Now.** `progress_percent_step`, default 10, so a 20,000-entry archive
produces at most 11 notifications. The test asserts the callback fires between
5 and 12 times for `many.zip`.

---

## 10. Open question 5 is still open

v2 section 11 asks whether a community payload already does part of this
(question 5; it was question 4 in v1). That check has not been done and is
worth doing before Milestone 3 — it is the cheapest question on the list to
answer and the most expensive one to get wrong.

---

## 11. New in v2, not yet implemented

Recorded here so they are not lost between drafts:

- **FR-6 verified delete — done in M3.** `extraction_is_verified()` checks
  the result, the entry count, the file count against the new
  `files_expected` counter, and the byte total, and fails closed on
  anything unexpected. Per-file sizes were already enforced inside
  `extract_one_file`. Three tests cover it: a verified delete, a corrupt
  archive that fails partway, and a hostile archive refused at preflight —
  the source survives both failures.
- **FR-2 peak-space note.** v2 spells out that peak usage is
  `archive_size + uncompressed_size` and that the preflight must not budget
  for space `delete_after_extract` reclaims afterwards. `check_space` already
  complies: it requires `uncompressed + margin` to be *free*, and the archive
  is already occupying its own bytes at that moment, so nothing subtracts
  them.
- **Web UI (M4) and watch mode (M3).** Not started. The extraction core takes
  a progress callback and holds no global state, so running it off the HTTP
  serving path (v2 section 7, non-blocking UI) needs no change here.

---

## 12. Zip filename encoding is undefined

**Gap.** Neither draft says how entry name bytes should be interpreted. Zip
carries a language-encoding flag (general purpose bit 11): when set the name is
UTF-8, when clear it is nominally CP437, and many writers ignore both and emit
the local codepage.

**Now.** Names are treated as opaque bytes and written unchanged, which is what
the console filesystem does anyway and what keeps a correct UTF-8 archive
correct. A CP437-encoded name will land as mojibake on the console.

**Still needed.** Probably nothing. Transcoding would need a CP437 table and a
policy for names that are neither, and section 9 already notes the console FTP
server cannot show non-ASCII names regardless. Worth stating in the spec that
pass-through is deliberate, so it does not get "fixed" later by accident.

---

## 13. 7z was out of scope, and now is not

**Change of scope, decided deliberately.** v2 puts 7z last in v2+ and calls
game installation an explicit non-goal. The actual workload is a single large
archive holding one disk image, where extracting on-console means transferring
29 GB over FTP instead of 152 GB.

That is the same objective the requirements state — faster transfers — reached
by a different mechanism. The stated rationale, per-file connection overhead,
does not apply to a one-file archive at all, so the spec's reasoning does not
cover this case either way. Recorded here so the decision is visible rather
than looking like scope drift.

**What it does not change:** extraction still needs the full uncompressed size
free at the destination, and the preflight still refuses when it is not there.
For a 152 GB image that is a real constraint, not a formality.

---

## 14. Solid archives break the "entries are independent" assumption

**Gap.** Nothing in either draft anticipates solid compression, where files
share one compressed block and have no individual entry point. Reaching the
fifth file means decoding and discarding the four before it.

**Now.** The sequential `ua_backend` interface already matched this: entries
come out in order, and the 7z backend keeps one decoder open per folder and
advances through it. A caller that skipped an entry would cause the skipped
bytes to be decoded and thrown away rather than seeked past, which is inherent
to solid compression rather than a defect.

**Still needed.** Nothing, but worth knowing before v2's resume feature is
designed: resuming mid-folder in a solid archive means re-decoding from the
start of that block, so a byte offset alone is not enough to resume cheaply.

---

## Bugs found while building, worth recording

### miniz filename truncation

`mz_zip_reader_get_filename` clamps the name length to the buffer it is given
*before* returning it (`third_party/miniz/miniz_zip.c:4864`), so a
"did it fit?" check against the return value can never fail. A 1204-character
entry name was silently accepted, truncated to 1023 characters, and would have
been written under a name the archive never contained. Fixed by querying the
length with a zero-size buffer first, which is the only call that reports the
true value.

This is the kind of defect the host harness exists to catch, and it would
have been very hard to see on-console.

### The harness was lying about non-ASCII filenames

`ua_plat_win.c` originally used the ...A Win32 entry points, which decode a
byte path through the process ANSI codepage. A zip entry named `café`,
stored correctly in the archive as the bytes `63 61 66 C3 A9`, landed on disk
as `63 61 66 C3 83 C2 A9` — the two UTF-8 bytes decoded as CP1252 and
then re-encoded, which renders as `cafÃÂ©`. The console, whose filenames
are plain bytes, would have stored the original unchanged.

The harness therefore disagreed with the target about what extraction
produces, which is the one thing a harness must not do. Worse, the test
asserted only `files_written == 4`, so it passed throughout.

Fixed by decoding UTF-8 explicitly and using the wide entry points, so
Windows stores the same characters the console would. `test_unicode_names`
now asserts each exact path, and asserts the double-encoded form is absent.

The lesson generalises: a count assertion cannot catch a corruption that
preserves the count.

### Decoded bytes were being thrown away between entries

The first 7z implementation decoded a chunk, handed the current entry as
much of it as that entry still wanted, and dropped the remainder. A single
decode round routinely spans several small files, so every file after the
first in a folder read from the wrong offset and the folder ran out early:
`archive ends 4 bytes before the entry does`.

Fixed by keeping the undelivered remainder buffered across entries, and
tracking the folder's logical position (what has been handed out) apart
from its decoded position (what has been produced). `solid.7z` checks all
eight files byte for byte, because the wrong-offset version still produced
files of plausible size.

A second bug in the same area: a decode round can legitimately consume
input and produce nothing, on an LZMA2 chunk header or dictionary reset.
That was read as end-of-stream. `folder_fill` now loops until it has a
byte or the folder genuinely ends.

### An unsupported filter chain was refused too late

BCJ2 folders were detected inside `folder_begin`, which only runs during
extraction — after preflight passed and the destination had been created.
FR-2 requires a failed preflight to leave nothing behind. The coder chain
is now classified during the preflight walk instead.
