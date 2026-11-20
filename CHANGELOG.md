# Changelog

All notable changes to Skim Master are recorded here. Versions follow
[Semantic Versioning](https://semver.org/); the receiver and decoder plugin
ABIs are versioned separately by their exported symbol names.

## [Unreleased]

### Added

- Per-mode decoder settings in `[decoders.<MODE>]` and per-slice `settings`
  tables, edited from the CW, RTTY and digital decoder pages: CW search range,
  channels and initial speed; RTTY shift, baud, reverse, channels and
  threshold; FT8/FT4 search range, candidates, sync score and LDPC
  iterations; WSPR passes and subtraction. Changes restart only the receiver
  workers that use the mode.
- Live channel text on the CW and RTTY pages, published by receiver workers
  to a transient `live_text` table (schema version 3) and polled every two
  seconds.
- **Decode a Recording** on the decoder pages: decode an uploaded WAV with the
  saved settings and show the results without storing or reporting them.
- Band & Mode Activity per the reference design: band × mode matrix with
  heat scale and normalization, hourly UTC heatmap, mode totals with share,
  unique calls and peak hour, period/mode/receiver filters and CSV export.
- Click-to-tune spectrum: click the trace to add a slice at that frequency;
  running slices are shaded on the trace.
- Noise Floor Analysis: one-minute noise-floor densities per receiver
  (`noise_floor`, schema version 4), a 24-hour chart, per-band statistics
  with trend and alert threshold, CSV export, and the latest value in the
  System Summary.
- Panel restore of an uploaded snapshot, a redacted diagnostics bundle and a
  confirmed factory reset. Each change is staged and applied by an in-process
  station restart that keeps a snapshot of the previous data.
- **Restart station** on Restart Daemon, applying listener and data path
  changes without restarting the server process.
- Worker kind, lifecycle state, PID, restarts, CPU and memory in the System
  Summary, along with active bands.
- Versioned station schema (`PRAGMA user_version`) with migrations on open.
- CI format check and AddressSanitizer/UBSan and ThreadSanitizer runs.

### Changed

- The spectrum uses averaged 2048-point FFTW frames and 512 display bins
  instead of a single 256-point transform.
- CSV exports keep negative numbers numeric; other cells starting with a
  formula character are still prefixed with an apostrophe.

- Slot decoder plugin ABI v3: `skim_slot_decoder_v3` replaces
  `skim_slot_decoder_v2`. Metadata declares settings, and `start` and the
  `SmBuiltinSlotDecoder` callback receive resolved values. Rebuild external
  decoders against the new SDK.
- Unhandled panel request errors are logged to stderr with their path.

## [0.1.0] - 2026-11-20

First tagged baseline.

- Wideband IQ from SoapySDR, RTL-TCP, PortAudio stereo IQ and IQ WAV receiver
  plugins, with a shared probe path for the wizard and receiver editor.
- One slicer per receiver fed by a bounded IQ queue; libcsdr channelizes each
  active slice onto bounded NNG queues.
- FT8, FT4 and WSPR slot decoder plugins; CW, RTTY and SSTV streaming decoders.
- Typed station object graph, digest-checked TOML saves, hot reload, manual
  allocations and UTC band schedules.
- Boost.Process supervision of per-receiver and per-service workers.
- SQLite storage, local Telnet DX Cluster, PSK Reporter, WSPRnet and RBN `rx/6`.
- Server-rendered control panel and setup wizard, SSTV gallery, CSV exports,
  ZIP snapshots and `SkimMasterTools` restore, replay and decode utilities.
- Installable C++17 plugin SDK.

[Unreleased]: https://github.com/goodspeed34/SkimMaster/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/goodspeed34/SkimMaster/releases/tag/v0.1.0
