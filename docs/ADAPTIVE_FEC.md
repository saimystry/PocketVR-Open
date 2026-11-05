# Automatic FEC protection

`fec.adaptive` defaults on. `fec.ratio_pct` is the configured protection floor;
automatic protection stays between it and 25 percent. Turn adaptive off for a
fixed ratio. Changes apply to subsequent complete frames without encoder reset
or reconnect. Actual parity is quantized down to bounded whole-group shards;
requested percent is not an exact per-frame overhead guarantee for tiny frames.

Only fresh, authenticated, clock-ready phone reassembly/recovery counter deltas
drive the controller. Active video with unrecovered reassembly drops raises the
requested ratio by seven percentage points at most once per two seconds. Useful
FEC recovery holds protection. Ten clean active seconds allow gradual retreat
to the user's floor. Duplicate, reset, missing, stale, idle or unsynchronized
reports cannot invent loss or clean recovery. These counters are not a measured
packet-loss percentage and can include late packets from scheduling pressure.

TCP adb mode always bypasses FEC, including when adaptive mode or a nonzero floor
is selected. Bitrate setter rejection does not disable FEC adaptation. Status
under network.status.session.adaptation.fec and the phone connection screen
distinguishes requested ratio, floor, automatic/fixed, reason and TCP bypass.

Verification: 157 Windows checks and the actual Pixel suite pass. Trace tests
cover bounds, cadence, useful recovery, clean retreat, counters/replay/staleness,
manual mode and TCP bypass. Real authenticated driver fixture feedback raises
UDP protection, applies live manual overrides and retains zero parity over TCP.
Actual tether UDP test: 1,377 decoded / 8,608 poses, one connection; a live fixed
20-percent override applied and original settings were restored. Actual adb TCP:
1,416 decoded / 8,678 poses, one connection, requested FEC zero and bypass true.
Both include headset enter/exit/re-entry without reconnecting.

Physical impairment at 1/3/8 percent random and burst loss still needs acceptance.
Automatic resolution and negotiated FPS changes are separate unfinished work;
this milestone implements FEC only and does not silently change video format.
