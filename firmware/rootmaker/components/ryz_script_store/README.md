# Script Store

`ryz_script_store` is the sole script-filesystem Interface for C adapters. It
owns validation, immutable source snapshots, SHA-256, stable bounded catalogs,
compare-and-swap writes, transactions and explicit recovery. It does not mount
or format SPIFFS, execute Lua, implement HTTP, or migrate user boot behavior.

## Admission and ownership

- Basenames: 1–36 ASCII letters/digits/`_`/`-`, followed by `.lua` (total <=40).
- New source: 1–16384 bytes, no embedded NUL. `get` applies the same source checks
  and returns an owned copy plus its SHA-256; release via `snapshot_free`.
- Only regular files are read/mutated. Host checks use no-follow filesystem
  operations; the production SPIFFS adapter has no symlinks or `lstat` symbol.
- `boot.lua` is an ordinary user file: it is visible, replaceable and removable,
  with `protected_file=false` like other regular scripts. System diagnostics
  belong to C-owned readonly assets outside this namespace. Storage changes do
  not themselves enable user autorun or choose its exit/rescue policy.
- One nonblocking exclusive lock covers all Store filesystem operations.
  Contention returns `ESP_ERR_TIMEOUT`. Hashing an independent immutable input
  via `source_sha256`, and freeing a completed snapshot, need no Store lock.
  Callers retain borrowed arguments unchanged until their call returns.
- The mounted directory must have no out-of-band writers. Application execution
  admission is a separate owner policy; the Store does not inspect Lua/jobs.
- Catalogs are sorted by ASCII `strcmp`, at most 256 entries, 16 entries/page.
  Revisions start at 1; optional revision comparison prevents mixing pages
  across Store mutations. Revision exhaustion rejects further mutation rather
  than wrapping. Adding a 257th regular script is rejected. Timestamp records
  are read only with source snapshots/descriptions, not by the catalog/Owner.
  Author/version parsing remains a separate module.
- CAS: NULL means unconditional, empty string requires absence, 64 lowercase
  hexadecimal characters require that exact old content SHA. `describe` returns
  raw identity for empty, NUL-containing or oversize regular files; these can
  be repaired/deleted through CAS mutation, but cannot be returned as runnable
  source by `get`. Raw identities use a 512-byte streaming buffer in the target
  adapter, never allocate the whole file, and are bounded by the opened file's
  measured length. Original invalid content can therefore be restored intact
  during rollback without pretending it is an executable script.

## Transaction and recovery

The short private names are `.ryz-txn`, `.ryz-new`, `.ryz-old`. The fixed-size
journal records its version, put/remove operation, validated target basename,
whether an old target existed, old/new content digests, and a digest of the
record. All fields are byte arrays, so the record is identical on 32/64-bit
platforms. This digest detects corruption; it is not an authentication scheme.
New transactions use `RYZST03` (673 bytes), including transactions targeting
`boot.lua`, and carry complete old/new time records. Recovery still accepts
246-byte `RYZST01`/`RYZST02` records; v1 is restricted to its original non-boot
targets. A v1 record targeting `boot.lua` remains invalid even with a matching
record digest: opening the user file must not reinterpret formerly forbidden
or damaged legacy recovery state as an authorized transaction.

For put, write/flush/close and read back the journal; exclusively create and
verify the stage; rename the old target to the backup if present; then rename
the stage to the now-absent target. Remove writes the journal then renames the
target to the backup. These final namespace changes are the **logical commit**.
The backup/stage are cleaned first, and the identity journal is removed last.
No rename is allowed to overwrite a destination, including journal updates.
The journal is not removed until its associated time record has also settled.

Recovery observes the verified journal and content identities:

| Observed state | Recovery |
| --- | --- |
| Put target matches new SHA; no stage | Committed; clean the verified backup and journal |
| Put target absent; verified old backup exists | Restore old target, then remove stage/journal |
| Put old target intact, no backup; or new-file target/backup both absent | Not committed; remove stage/journal |
| Remove target absent; backup absent or matches old SHA | Committed; clean backup/journal |
| Remove old target intact; no backup | Not committed; clean journal |
| Corrupt/partial journal, unexpected target/backup, nonregular artifacts, or orphan stage/backup | Preserve everything; explicit recovery/manual reconciliation required |

An I/O error may occur after a namespace operation. The core immediately tries
identity-based recovery and returns both the original error and observed commit
state. Callers must inspect the result even when `esp_err_t != ESP_OK`:

- `COMMITTED`: requested content/absence is visible; **do not blindly retry**.
  A nonzero `cleanup_error` and `recovery_required` can coexist with committed.
- `NOT_COMMITTED`: the transaction did not establish the requested mutation.
- `COMMIT_UNKNOWN`: do not claim success or rollback; writes remain blocked
  until recovery or explicit external reconciliation establishes the outcome.

Startup `init` performs recovery, and same-root `init`/`recover` can be repeated.
Once the mount is available, a recovery error leaves `ready=true`: status,
catalog and readable named files remain available while writes are blocked.
An unavailable root has `ready=false`. Unknown legacy `.upload.bak` and
`.upload.tmp` are never deleted or assigned a guessed target; their presence
blocks writes until explicitly reconciled outside the Store.

Status is a pure in-memory copy: it never calls SPIFFS or waits on its garbage
collector. Capacity is refreshed in the non-UI init/mutation/recovery paths and
exposed with `capacity_valid` and `capacity_error`; an invalid sample must not
be rendered as an empty filesystem. UI/status callers retain their old sample
when the nonblocking Store lock is busy. A typed permanent platform I/O fault,
including one observed during get/list/describe or mutation preflight, sets
`recovery_required` without confusing normal missing files or CAS conflicts
with global failure.

## Startup copy and Delete All

`copy_boot(source_name, source_sha256, expected_revision, result)` validates a
nonzero current revision and the selected source's exact SHA under the same
exclusive lock that reads and copies it. It uses the existing put transaction,
not an externally separated get/put pair. The selected source must be valid
1–16384-byte runnable source; empty, NUL-containing and oversized files cannot
be promoted to startup source. A self-copy or an already identical `boot.lua`
is an explicit `COMMITTED` no-op without a write or revision increment. The
copy does not execute the script, restart the device, or establish autorun.

`delete_all(expected_revision, result)` freezes the sorted bounded catalog
under that same lock, then runs one normal remove transaction per regular Lua
file. It includes user `boot.lua` and files with invalid source bytes, but not
invalid names, nonregular entries, private transaction files or unrelated data.
A stale/zero revision, an oversized index, or insufficient initial transaction
reserve causes no deletion. The caller must separately exclude application
starts; the entire operation belongs on a filesystem worker, not the UI Owner.

This is **not a whole-batch atomic transaction**. `total` is available only
after a complete initial scan; `removed` counts individually confirmed commits.
The first error, unknown commit or cleanup failure stops the batch, while
earlier confirmed deletions remain effective. `failed_name` identifies that
file when an individual operation failed. A last-file cleanup failure can
return `complete=true` together with a nonzero error and `recovery_required`:
all selected files are visibly absent, but storage cleanup is not healthy.
An unknown result is never counted as removed or reported complete. Callers
must inspect the error, counts, `outcome_unknown` and recovery fields together,
and must not automatically retry an uncertain confirmation.

After interruption, ordinary Store recovery settles only the single recorded
file transaction. It never continues deleting the remaining catalog. A further
batch needs an explicit new request with the currently observed revision.

## Actual SPIFFS limits

The ESP adapter is restricted to the already-mounted `scripts` partition at
`/scripts`. SPIFFS is flat: root availability is checked through its mount state,
not a fictitious POSIX `stat('/')` directory. Directory entries are checked via
`stat`, not SPIFFS's non-POSIX `d_type`; enumeration errors are not treated as EOF.

The current `CONFIG_SPIFFS_OBJ_NAME_LEN=32` includes the VFS's leading slash and
terminator, so this target supports basenames up to **30 bytes**, even though
the protocol grammar permits 40. Overlong names return `INVALID_SIZE`; this
module never silently shortens them, changes the configured limit, reformats
the disk, or invokes the mutating `esp_spiffs_check` repair routine.

Capacity admission retains the old file and requires free bytes for the new
stage, 673-byte journal, 213-byte timestamp stage, and an additional 4096-byte
reserve (also reserved by remove/bulk admission). This is conservative admission, not a guarantee against SPIFFS GC or
metadata allocation failure. All files are closed before rename, which itself
may consume an internal descriptor; the existing four-descriptor mount limit
is not increased.

`fsync` on this target flushes one SPIFFS descriptor. Neither it nor `rename`
provides a proven all-power-loss transaction guarantee. A damaged journal or
lost file after power loss may require manual recovery. Real controlled
power-cut testing is separate from Host fault injection. If a target close
fails, IDF may already have removed the VFS descriptor while retaining an
internal SPIFFS descriptor: the adapter enters sticky I/O degradation until
system restart, and never retries a possibly-reused numeric descriptor.

## Platform seam and verification boundary

`ryz_script_store_platform.h` is private. The ESP adapter supplies bounded
filesystem operations, actual capacity, mbedTLS SHA-256 and PSRAM allocation.
Host tests substitute real temporary-directory I/O and SHA with fault injection
at this same seam, exercising the production transaction/state logic. Host
success is not evidence of SPIFFS power-loss durability or hardware behavior.

## Trusted UTC timestamps

`get` and `describe` return `times.created_unix` / `times.modified_unix` with
the same source identity, under the same lock. Each is independently zero when
unknown. Valid stored seconds cover 2024-01-01 through 9999-12-31 UTC. Existing
files and factory images have no inferred creation date; raw SPIFFS mtime,
source comments, build time and the developer's clock are never promoted.

The ESP adapter samples `ryz_time_sample_utc` once at mutation preparation.
This is a non-waiting calibrated UTC sample, not the last NTP synchronization
instant. Offline calibrated time is usable; uninitialized, busy or invalid
time produces unknown without rejecting an otherwise valid file write. A real
overwrite preserves a known creation time but replaces modified time with this
sample, including unknown. Identical-content puts/self-copies do not sample or
write anything. A boot copy applies the destination's rules, not the source's
creation date. A successful deletion removes the time record; recreating the
same name/content starts a new creation history. Recovery never resamples time.

One hidden `.ryz-t` + 24 lowercase filename-SHA hex sidecar per file fits the
actual 30-byte SPIFFS basename limit. Its fixed 213-byte `RYZTM01` record holds
the full basename, source SHA, canonical 16-hex-digit UTC values and checksum.
The complete basename detects prefix collisions. Missing records mean unknown;
malformed/mismatched records are preserved and block writes. Reads retain raw
source-maintenance access but hide dates while recovery is required. No-journal
init/recovery checks this bounded private namespace, including source identity;
unrelated hidden files are not included.

After the source commit, the verified V3 journal installs the intended record
through `.ryz-time-new`: exclusive write, readback, remove only a verified old
record, then non-overwriting rename. Canonical records must byte-match the
journal's old/new record; unknown artifacts are never overwritten. With a valid
journal, only a full or partial exact prefix of its old/new record is a
repairable timestamp stage. An orphan stage without a journal is preserved.
Rollback reinstates the journal's old record (or its absence), whereas commit
installs the new one or removes it for deletion. Only then is the journal
cleaned last. Metadata failure returns COMMITTED + cleanup_error/recovery when
the source is visibly committed; this is not full success or safe-to-retry
failure. Existing RPC `ok` describes source commitment and additionally carries
`store_cleanup_error` / `store_recovery_required`. Dates remain null until clean.

V1/V2 recovery with a timestamp sidecar or time stage is rejected as a mixed
format state. Completed writes by old firmware or any out-of-band writer are
**not supported**: in particular, old firmware could delete/recreate identical
bytes without changing a surviving sidecar, which SHA alone cannot detect.
The checksum is corruption detection, not authentication. No SPIFFS geometry or
partition is changed and no existing files are automatically reformatted.

Current implementation, Host recovery checks, original-V5 screenshots and
target-build evidence: [file time increment](../../../../docs/software/firmware-v5-file-times.md).
