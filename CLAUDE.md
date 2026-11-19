# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Skim Master is a C++17 daemon that reads wideband IQ from radio front ends or recordings, allocates independent decoder slices, stores receptions and serves a web control panel. It supports CW, RTTY, FT8, FT4, WSPR and SSTV.

## Implemented

- **Server and configuration.** `SkimMaster [STATION.toml]` takes one optional TOML path. With no argument it uses `config.toml` in the working directory and creates it from an embedded sample if missing. Listener address, port and first-run wizard flag are top-level settings. Station identity, receivers, slices, schedules and reporter settings live in the same TOML. Startup, listener and worker lifecycle messages go to stderr. The System Log displays controller and worker lifecycle events stored in SQLite.
- **Supervision and scheduling.** Boost.Process supervises the web process and per-receiver and per-service workers. Receivers, services and UTC schedules reload from valid TOML edits. Manual allocations take precedence during automatic scheduling. Among active schedules, lower priority numbers win and list order resolves ties. **Band Schedules** manages schedules with daily or weekday selection, duplicates and 30-minute **Run Now** overrides. Changes to the listener or data paths require a full server restart. The panel's restart action recycles receiver and service workers.
- **IQ sources and WAV playback.** SoapySDR, RTL-TCP, stereo ALSA on Linux and stereo IQ WAV use receiver modules. Add a recording with `wav:PATH` in the wizard or TOML and supply the WAV sample rate and recorded RF center. The path is on the server. Relative paths and `~/` are supported. The wizard selects repeat by default. Repeating playback is paced at the recording rate and resets decoder state at each wrap. One-shot playback drains the pipeline at EOF. File receptions appear in the database and panel and are excluded from every reporting feed, including the local DX Cluster. Further receiver sources are planned; see **Remaining work and verification**.
- **Slice and decoder pipeline.** libcsdr translates and resamples receiver IQ. Bounded in-process NNG queues pass IQ blocks to the decoder threads, which reset on discontinuities. CW uses libmorsedec across a 48 kHz IQ slice. RTTY searches a 12 kHz IQ slice for 45.45-baud ITA2 signals with 170 Hz tone spacing. Both use the source RF center by default and accept a per-slice `center_hz`. For existing TOML files, legacy CW or RTTY `dial_hz` is interpreted as the slice center when `center_hz` is absent. FT8, FT4 and WSPR retain a USB dial and run in process through decoder libraries. The SSTV decoder accepts live IQ and writes Martin 1/2, Scottie 1/2 and Robot 36/72 images to the gallery.
- **Storage and reporting.** SQLite stores spots, live-reception eligibility and reporting receipts. Local Telnet DX Cluster, PSK Reporter and WSPRnet are implemented. RBN uploads directly through the `rx/6` API with handshake, spot filtering, cadence, quota and endpoint failover. Reporting pages configure opt-in services, show supervised worker state and receipt records, and can restart one service worker at a time. PSK Reporter receipts record UDP dispatch; its protocol has no remote acknowledgement.
- **Setup and panel.** The wizard collects callsign, email, name and Maidenhead grid as station identity, then configures an IQ source and one decoder slice. CW and RTTY omit the dial field. Tuning errors are shown in the wizard and preserve entered values. Add / Edit Receiver lists sources and supports structured receiver and slice edits with TOML persistence. The editor and wizard both expose the per-driver probe and a manual device parameter. The editor can test whether the entered hardware settings produce IQ within three seconds. Station Settings exposes the full station TOML. The panel has no login and defaults to a loopback listener. Legacy authentication keys are ignored and removed on save.
- **Readouts and recovery.** The panel shows station summary, receiver status with separate RF, rate, gain, slice and last-IQ fields, schedule and slice tables, recent spots, heard stations, receiver/mode counts, reporting status, system log and an SSTV gallery. Spectrum/waterfall and heard-grid views poll data from the daemon. The CW, RTTY and digital pages show active allocations and up to 200 recent stored results with receiver/mode filters, refresh pause and CSV export. The gallery filters by SSTV mode and UTC date, exports a matching CSV index and deletes individual images. The panel downloads ZIP snapshots of the SQLite database and SSTV gallery. `SkimMasterTools` provides validated offline restore and replay utilities. Keep the TOML separately from the snapshot.

## Remaining work and verification

| Area | Current gap |
|---|---|
| Noise Floor Analysis | Intentionally displays **Unimplemented**. Spectrum readings are relative dBFS and do not provide a noise-floor analysis. |
| Decoder and network controls | Decoder pages show active allocations and recent stored results. Per-mode settings, live channel/text controls and on-demand decode remain pending. Reporting pages configure opt-in services and restart an individual service worker; receipt counts reflect recorded sends or acceptances. |
| Monitoring and statistics | Spectrum lets the operator choose an IQ source, and the DX map plots heard grids on a coordinate grid. Spot, heard-station, band/mode and decoder-result pages support their available filters and CSV export. Live spot and decoder pages can pause refresh. Interactive spectrum tuning, detailed RF-band statistics, diagnostic bundles and noise-floor analysis are pending. CPU/memory, active-band counts and several diagnostics still have placeholder values. |
| Gallery and system tools | The gallery displays the 100 newest images with mode/date filters, individual deletion and filtered CSV index export. System logs and decoder/reception tables export CSV. Diagnostics bundles, browser restore and factory reset are pending. A full server restart is done outside the panel. |
| Additional receivers | Planned, not implemented: **FlexRadio**, **SunSDR (TCI)** and **SDR Console (DataRepeater)** network sources, plus **PortAudio** sound-card input on all platforms, which would generalize the Linux-only stereo ALSA module. Each would be a separate receiver module behind the `receiver/api.h` plugin boundary. |
| Deployment validation | Linux is covered by the local build and tests. Win64 CLANG64 binaries and protocol/decoder suites have run under Wine. Native MSYS2 execution, hosted CI, physical SDR interoperability and production reporting acceptance remain unverified. Test runs use fixtures, emulated RTL-TCP and local reporter endpoints. |

## Source layout

All production sources live under `src/`. Use it as the include root, with qualified paths such as `"decoder/slot.h"`. Keep `Spot` and other shared domain types in `core/types.h`, slot and streaming decoder entry points in `decoder/`, and `SaveSlot` in `store/`. Core has no dependencies on other project layers. Receiver and decoder interfaces consume core types and expose their own versioned plugin boundaries in `receiver/api.h` and `decoder/api.h`. The application coordinates those layers with DSP, persistence, reporting and the web panel.

## Licensing

libcsdr is mixed-license: the core (`libcsdr.c`, `fft_fftw.c`, `fastddc.c`, `ima_adpcm.c`) is BSD-3-Clause, but `libcsdr_gpl.c` is GPL-3.0-or-later. The wrap builds it with `-DLIBCSDR_GPL` ("GPL on"), so the resulting binary must be distributed under GPL-compatible terms.

**csdr C++ caveat:** the csdr headers have no `extern "C"` guards. From C++, include them as `extern "C" { #include <libcsdr.h> }` to avoid C++ name mangling at link time.

## Build & test

The Linux build passes all 17 Meson tests. Coverage includes IQ replay, multichannel CW, off-center RTTY, FT8/FT4 and WSPR slots, all six SSTV modes, repeating WAV and reporting exclusion, reporter wire formats, decoder page filters/export, gallery mode/date filters, CSV, deletion and symlink checks, service opt-in/configuration and local worker restart, TOML identity fallback and field preservation, recovery from malformed TOML, WAV and RTL-TCP discovery, receiver edits and schedules, station lifecycle and snapshot validation. See [README.md](README.md) for build and operating commands.

## Code style & tooling

- **Google C++ Coding Style** — enforced by `.clang-format` (`BasedOnStyle: Google`).
- **Simple, plain C++17** — no "fancy" C++20 constructs; `cpp_std=c++17` is pinned in `meson.build`.
- **clang-format** with a git pre-commit hook that rejects staged `.cpp`/`.h`/`.hpp` files that don't match. The hook lives in `.githooks/pre-commit` and is activated via `git config core.hooksPath .githooks` (already set locally). It silently passes when `clang-format` is absent or no C++ files are staged.

## Architecture

See [ARCHITECTURE.md](ARCHITECTURE.md) for the receiver → slice → decoder → spot pipeline, plugin contracts, thread and message ownership, storage and lifecycle.

## ReferenceDesign

`ReferenceDesign/` defines the target look and intended controls. At build time, `src/web/assets/embed.py` uses the reference shell, stylesheet, navigation and help to generate embedded Inja templates. Production pages have server-rendered routes, with compatibility for reference hash links. They use daemon data and a small set of scripts for the wizard, shell and charts.

Preserve the reference header, left navigation and right help column, along with its dense bordered tables, indigo and orange palette, silver gradients and monospace data values. Adapt the center column to the daemon's actual capabilities. The mockup's simulated data and buttons are design references.
