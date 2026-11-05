# Desktop diagnostics ZIP export

In the desktop control app, choose **Export diagnostics ZIP** and select where
to save `PocketVR-diagnostics.zip`. The desktop takes a fixed copy of the current
status before opening the file picker, then builds the bundle on a separate
worker. Pairing, status polling and streaming do not wait for log collection.
Export remains available when SteamVR is stopped; the snapshot records that the
driver is unavailable and includes eligible local logs.

The archive contains:

- `diagnostics.json`: driver status, network/session/phone metrics and settings.
- `manifest.json`: UTC capture time, bounds, source log sizes, truncation,
  filtered-line counts and missing/unreadable-log reasons.
- `logs/`: complete filtered records from recent tails of known PocketVR
  `driver`, `control`, `input` and `overlay` logs and their three rotations, when available.
- `README.txt`: contents, privacy limits and measurement interpretation.

Pairing status/codes, peer lists and identity stores are excluded. Collection
does not scan arbitrary files or subdirectories; video captures, APKs, keys and
DPAPI stores are not eligible. Reparse log files, files reported as multiply
hard-linked and linked log directories are skipped. Hard-link detection relies
on filesystem metadata; the network share reports one link even for known
hard links, so that protection is not established for such shares. The normal
local Windows log directory is the intended source. Suspicious secret-bearing lines,
unstructured records, binary/control data, overlong lines and partial records
are omitted. The JSON filter also excludes nested identity/credential fields.
These filters cannot recognize every possible secret: review addresses,
settings and log contents before sharing. The export stays local and uploads
nothing.

Each log contributes at most its last 512 KiB; the entire ZIP is bounded to
8 MiB. Log payload has a separate 6 MiB aggregate budget, with current logs
prioritized before older rotations so metadata still fits. The manifest identifies shortened logs, so absence of an older message
is not evidence it never happened. Reading a rotating/in-use log is best effort;
unavailable files do not prevent an otherwise useful export. Logs may be absent
on a PC that has never run the driver. Stale phone stats retain their existing
freshness indicators. Counters and encode/decode timings are not physical
motion-to-photon or scan-out latency measurements.

Saving writes a temporary sibling and replaces the destination only after the
complete archive has been written and flushed. Failure/cancellation retains the
previous destination and removes the temporary file. Closing the desktop asks
the worker to cancel. No collection or archive work runs inside the VR/video
callbacks. The ZIP uses the standard STORE method without a new dependency or
external archiver; ordinary ZIP readers can extract it.

Behavioral checks cover nested secret exclusions, offline export, bounded log
tails, non-log files, hard-linked logs, corrupt/unstructured/partial log records,
unsafe/duplicate archive paths, oversized archives, cancellation and failed
replacement cleanup. An independent Python `zipfile` check reads a native
export, validates every CRC and checks expected contents and exclusions.

Builds and test artifacts on the build PC remain under
`<build root>`; verification logs use
`<build root>`. This PC has no Steam/SteamVR, so
automated checks here do not establish live VR performance or hardware acceptance.
