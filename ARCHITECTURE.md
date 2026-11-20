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

Bundled receiver modules expose `SmReceiverPlugin` through `skim_receiver_v2`. The host discovers modules at runtime. Receiver plugins advertise their ID and publish a prefix, label and capabilities through `skim_receiver_descriptor_v2`. The host selects a slot decoder by asking candidate modules whether they support the requested mode. A probe takes a driver-specific target and reports device parameters, labels and sample rates when available. The wizard and receiver editor use the same probe path. The v2 interface uses the same C++ toolchain compatibility model as the slot decoder plugins.

Each bundled plugin lives under `plugins/` with its own build target. Plugin targets include the public headers under `sdk/` and link their own dependencies. The host discovers `librcvr*` and `libdecoder*` shared modules next to the executables or under their `plugins/` directory. It checks build-tree modules first, then colocated modules, with paths sorted within each group. Install one receiver per ID and prefix and one slot decoder per mode.

Receiver edits use the station TOML revision check and atomic save. The supervisor applies the resulting allocation changes.

## Station objects

`LoadSettings()` reads and validates the station TOML into `SmInstanceSettings`. `SmInstance::FromSettings()` builds the station object graph from those settings, and `SmInstance::Run()` supervises the worker processes. A receiver exposes its driver and slices, a slice exposes its decoder, and each child can navigate to its parent. The instance can also find receivers, slices, decoders, and services by ID. The configured graph includes disabled receivers and slices outside the current schedule or passband.

Lookup handles keep their graph snapshot alive across a configuration reload. Each receiver process builds its own snapshot from its active SQLite plan, using the same IDs as the supervisor. Capture, channelizers, and decoder sessions live in the worker. A timed result resolves its decoder through that worker snapshot and uses the slice's RF reference.

## Runtime and timing

The Boost.Process supervisor reads the station TOML file and starts one worker process per active receiver or service. A receiver session keeps the driver loaded through probe or capture. Capture runs synchronously in the receiver process and owns its device handle. The capture callback copies each wideband IQ block into a bounded SPSC queue. One slicer thread per receiver reads that queue, measures the wideband spectrum, and uses libcsdr to translate, filter and decimate the input for each active decoder slice. The resulting blocks move over bounded in-process NNG pair sockets to decoder input threads. Live capture drops wideband blocks when the queue is full. Generation changes signal capture gaps and wideband drops, while slice sequence numbers detect drops on individual output streams. Playback waits for queue space and drains each pass at EOF.

SoapySDR provides access to supported hardware including Airspy HF+. RTL-TCP, PortAudio stereo input and stereo IQ WAV have their own receiver modules. WAV playback drains the slicers at EOF and is recorded as replay. FT8/FT4 and WSPR modules expose C++ slot decoder classes with identity and input metadata. Each decoder starts a worker thread that receives complete slots over a bounded SPSC queue and reports offsets over NNG. The receiver worker’s main thread resolves the decoder ID through its fixed slice plan, converts offsets to RF, and commits the results. CW uses libmorsedec. RTTY processes ITA2 frames at 45.45 baud and detects pairs spaced 170 Hz apart. SSTV writes completed Martin, Scottie and Robot images to the gallery.

Capture uses device timestamps to detect discontinuities when available. Live timed slots anchor sample time to host UTC, discard incomplete windows and check synchronization before accepting new samples. The slot periods are 7.5 seconds for FT4, 15 seconds for FT8 and 120 seconds for WSPR. Spectrum frames provide relative dBFS readings.

## Configuration and lifecycle

The setup wizard and admin settings submit the expected TOML content digest. Validation checks receiver identity, tuning, rates and slice definitions before atomically replacing the TOML file. The station stores slice and decoder IDs in TOML and carries their links to the receiver into each SQLite worker plan. For each tuning plan the controller starts all attached slices whose full passbands fit inside the captured IQ bandwidth. The receiver can supply spectrum data even when all slices are waiting for coverage. SQLite holds worker state and the restart generation. Manual allocations take precedence over schedules. Among active schedules, lower priority numbers win and list order resolves ties. The controller waits for the previous process to exit before launching a replacement for a changed allocation. Failed workers retry and record their status in the system log.

The supervisor stays up during a receiver or service restart. SQLite transactions protect committed receptions and reporting receipts across worker restarts. ZIP snapshots contain a manifest, a SQLite snapshot and gallery images. Restore validates the manifest and database before copying station data. The operator keeps the TOML configuration separately.

## Storage and delivery

The supervisor uses sqlite_orm to read and write typed worker rows in SQLite. The station initializes the schema with explicit SQL, and the panel can expand worker plans with SQLite's JSON functions. Reception writes, reporting, and snapshot operations use the SQLite C API.

Spots store receiver, mode, observed UTC, RF frequency, message and available callsign and grid. The transaction also records eligibility and optional measurements for live receptions. Public reporters select recent live receptions and persist acknowledgements or UDP send receipts. The local DX Cluster reads newly committed live receptions and bounds each client's output queue. RBN uses the direct `rx/6` API, including handshake configuration, filtering, cadence, quota, endpoint failover and optional confirmation feedback.

See [README.md](README.md) for build and operation. Tests use saved and synthesized signals, an emulated RTL-TCP receiver, and local HTTP and UDP endpoints.
