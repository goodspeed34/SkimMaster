# Skim Master architecture

Skim Master converts wideband IQ into independently decoded slices, records receptions and publishes eligible spots. The [control panel mockup](ReferenceDesign/index.html) and [setup wizard mockup](ReferenceDesign/wizard.html) define the operator experience. The application renders its own Inja views with the reference stylesheet.

## Source boundaries

| Directory | Responsibility |
|---|---|
| `src/core` | Shared spot and IQ types, SQLite connection utilities and clock state |
| `src/dsp` | Spectrum telemetry and serialization of FFTW planning |
| `src/receiver` | IQ input and the receiver plugin ABI |
| `src/decoder` | Slot and streaming decoders and the decoder plugin ABI |
| `src/store` | Transactional reception writes |
| `src/report` | Telnet, PSKReporter, WSPRnet and direct RBN |
| `src/web` | Read models, admin handlers and embedded assets, with one route module per panel area sharing `web/common.h` |
| `src/app` | CLI, supervisor, schedules, pipeline orchestration, backup and replay |

Core headers have no dependencies on other project layers. Receivers expose IQ, while decoders consume IQ and emit domain data. The application supplies the stream and persists the results. Web handlers call the station configuration API and read committed state through SQLite.

Bundled receiver modules expose `SmReceiverPlugin` through `skim_receiver_v3`. The host discovers modules at runtime. Receiver plugins advertise their ID and publish a prefix, label and capabilities through `skim_receiver_descriptor_v3`. `Capture` writes to an `SmCaptureSink`. Besides samples and gaps, a plugin with `reports_state` reports the center, rate and gain the device applied. The receiver session logs each change. When the center or rate differs from the worker plan, it signals one gap and drops IQ until the device is back on plan, so a radio retuned from its own controls never feeds slices at the wrong frequency. Devices with several IQ streams expose each one as a separate device string. The host selects a slot decoder by asking candidate modules whether they support the requested mode. A probe takes a driver-specific target and reports device parameters, labels and sample rates when available. The wizard and receiver editor use the same probe path. The v3 interface uses the same C++ toolchain compatibility model as the slot decoder plugins.

Each bundled plugin lives under `plugins/` with its own build target. Plugin targets include the public headers under `sdk/` and link their own dependencies. The host discovers `librcvr*` and `libdecoder*` shared modules next to the executables or under their `plugins/` directory. It checks build-tree modules first, then colocated modules, with paths sorted within each group. Install one receiver per ID and prefix and one slot decoder per mode.

Receiver edits use the station TOML revision check and atomic save. The supervisor applies the resulting allocation changes.

## Station objects

`LoadSettings()` reads and validates the station TOML into `SmInstanceSettings`. `SmInstance::FromSettings()` builds the station object graph from those settings, and `SmInstance::Run()` supervises the worker processes. A receiver exposes its driver and slices, a slice exposes its decoder, and each child can navigate to its parent. The instance can also find receivers, slices, decoders, and services by ID. The configured graph includes disabled receivers and slices outside the current schedule or passband.

Lookup handles keep their graph snapshot alive across a configuration reload. Each receiver process builds its own snapshot from its active SQLite plan, using the same IDs as the supervisor. Capture, channelizers, and decoder sessions live in the worker. A timed result resolves its decoder through that worker snapshot and uses the slice's RF reference.

## Runtime and timing

The Boost.Process supervisor reads the station TOML file and starts one worker process per active receiver or service. A receiver session keeps the driver loaded through probe or capture. Capture runs synchronously in the receiver process and owns its device handle. The capture callback copies each wideband IQ block into a bounded SPSC queue. One slicer thread per receiver reads that queue, measures the wideband spectrum, and uses libcsdr to translate, filter and decimate the input for each active decoder slice. The resulting blocks move over bounded in-process NNG pair sockets to decoder input threads. Live capture drops wideband blocks when the queue is full. Generation changes signal capture gaps and wideband drops, while slice sequence numbers detect drops on individual output streams. Playback waits for queue space and drains each pass at EOF.

SoapySDR provides access to supported hardware including Airspy HF+. RTL-TCP, PortAudio stereo input and stereo IQ WAV have their own receiver modules. WAV playback drains the slicers at EOF and is recorded as replay. FT8/FT4 and WSPR modules expose C++ slot decoder classes with identity and input metadata. Each decoder starts a worker thread that receives complete slots over a bounded SPSC queue and reports offsets over NNG. The receiver worker’s main thread resolves the decoder ID through its fixed slice plan, converts offsets to RF, and commits the results. CW uses libmorsedec. RTTY processes ITA2 frames, by default at 45.45 baud with tones spaced 170 Hz apart.

Decoders declare their operator settings as `SmSetting` entries (`sdk/core/settings.h`): built-in CW and RTTY in `decoder/text_modes.cpp`, slot decoders in their `skim_slot_decoder_v3` metadata. `decoder/settings.cpp` resolves a `[decoders.<MODE>]` table, or a slice's own `settings` table, against that schema. The station validates it on load, and every slice in a worker plan carries the table that applies. A settings change therefore alters the plan and restarts only the affected receiver workers. Workers pass the resolved `SmSettingValues` to the decoder.

CW and RTTY decoders also report a `SmTextChannel` snapshot of each active channel's recent text about once a second of input. The receiver worker resolves the offsets to RF and `SmLiveTextWriter` (`store/live_text.cpp`) replaces the slice's rows in `live_text`, stamped with wall-clock time, and deletes them when the slice stops. The panel reads rows updated in the last minute. Unlike `spots`, this table is a transient view and is not part of reporting.

On-demand decodes from the panel go through `DecodeRecording` (`decoder/recording.cpp`) in the web process. Slot modes reuse `DecodeSlot` and the installed plugin. CW and RTTY feed the WAV through `DecodeText`. Results are rendered on the decoder page and never reach `SaveSlot`. SSTV writes completed Martin, Scottie and Robot images to the gallery.

Capture uses device timestamps to detect discontinuities when available. Live timed slots anchor sample time to host UTC, discard incomplete windows and check synchronization before accepting new samples. The slot periods are 7.5 seconds for FT4, 15 seconds for FT8 and 120 seconds for WSPR. Spectrum frames provide relative dBFS readings. `SmSpectrum` averages up to sixteen 2048-point FFTW frames from the start of each second into a 512-bin `spectra` row. Rows are kept for an hour. It also takes the median of the unoccupied bins (the central 80%, excluding bins more than 6 dB above their median) and converts it to a density in dBFS/Hz. One-minute averages of that density go to `noise_floor` and are kept for seven days. The Noise Floor page and System Summary read that table.

## Configuration and lifecycle

The setup wizard and admin settings submit the expected TOML content digest. Validation checks receiver identity, tuning, rates and slice definitions before atomically replacing the TOML file. The station stores slice and decoder IDs in TOML and carries their links to the receiver into each SQLite worker plan. For each tuning plan the controller starts all attached slices whose full passbands fit inside the captured IQ bandwidth. The receiver can supply spectrum data even when all slices are waiting for coverage. SQLite holds worker state and the restart generation. Manual allocations take precedence over schedules. Among active schedules, lower priority numbers win and list order resolves ties. The controller waits for the previous process to exit before launching a replacement for a changed allocation. Failed workers retry and record their status in the system log.

Each `workers` row records the worker's kind (receiver, service, panel or controller), a lifecycle state (starting, running, exited, finished or failed), operator-facing status text, PID, launch time, restart count and the CPU and resident memory the supervisor samples every five seconds. The supervisor stays up during a receiver or service restart. A full restart request makes `SmInstance::Run` stop every process and return, and the server runs it again from the TOML file. SQLite transactions protect committed receptions and reporting receipts across worker restarts. ZIP snapshots contain a manifest, a SQLite snapshot and gallery images. Restore validates the manifest and database before copying station data. The panel stages an uploaded restore or a factory reset next to the database and requests a full restart; the server applies it between station runs, while no supervisor holds the station lock, and records the result in the system log. The operator keeps the TOML configuration separately.

## Storage and delivery

`core/schema.cpp` owns the station schema as numbered migrations recorded in `PRAGMA user_version`. Every process that writes the database migrates it on open, a database from a newer build is refused, and restore verifies a snapshot's tables and unique constraints against a freshly migrated reference. The supervisor uses sqlite_orm to read and write typed worker rows, and the panel can expand worker plans with SQLite's JSON functions. Reception writes, reporting, and snapshot operations use the SQLite C API.

Spots store receiver, mode, observed UTC, RF frequency, message and available callsign and grid. The transaction also records eligibility and optional measurements for live receptions. Public reporters select recent live receptions and persist acknowledgements or UDP send receipts. The local DX Cluster reads newly committed live receptions and bounds each client's output queue. RBN uses the direct `rx/6` API, including handshake configuration, filtering, cadence, quota, endpoint failover and optional confirmation feedback.

See [README.md](README.md) for build and operation. Tests use saved and synthesized signals, an emulated RTL-TCP receiver, and local HTTP and UDP endpoints.
