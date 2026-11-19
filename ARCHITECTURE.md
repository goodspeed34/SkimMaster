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
| `src/web` | Read models, admin handlers and embedded assets |
| `src/app` | CLI, supervisor, schedules, pipeline orchestration, backup and replay |

Core headers have no dependencies on other project layers. Receivers expose IQ, while decoders consume IQ and emit domain data. The application supplies the stream and persists the results. Web handlers call the station configuration API and read committed state through SQLite.

Each bundled receiver module exports `skim_receiver_probe_v1` alongside the existing capture ABI. A probe takes a driver-specific target and reports device parameters, labels and sample rates when available. The wizard and receiver editor use the same probe path. Receiver edits use the station TOML revision check and atomic save. The supervisor applies the resulting allocation changes.

## Runtime and timing

The Boost.Process supervisor reads the station TOML file and starts one worker process per active receiver or service. A receiver process loads the driver using Boost.DLL and owns its capture handle. The capture thread uses libcsdr to translate, filter and decimate each slice. The resulting blocks move over bounded in-process NNG pair sockets to decoder threads. Generations and block sequence numbers let decoders discard partial state following a capture error or dropped block. Replay uses bounded waits and exercises the same continuity rules.

SoapySDR provides access to supported hardware including Airspy HF+. RTL-TCP, Linux stereo ALSA and stereo IQ WAV have their own receiver modules. WAV playback drains the slicers at EOF and is recorded as replay. Each module exposes a versioned C function table. FT8/FT4 and WSPR also load as versioned C modules and execute in process. CW uses libmorsedec. RTTY processes ITA2 frames at 45.45 baud and detects pairs spaced 170 Hz apart. SSTV writes completed Martin, Scottie and Robot images to the gallery.

Capture uses device timestamps to detect discontinuities when available. Live timed slots anchor sample time to host UTC, discard incomplete windows and check synchronization before accepting new samples. The slot periods are 7.5 seconds for FT4, 15 seconds for FT8 and 120 seconds for WSPR. Spectrum frames provide relative dBFS readings.

## Configuration and lifecycle

The setup wizard and admin settings submit the expected TOML content digest. Validation checks receiver identity, tuning, rates and slice definitions before atomically replacing the TOML file. For each tuning plan the controller starts all attached slices whose full passbands fit inside the captured IQ bandwidth. The receiver can supply spectrum data even when all slices are waiting for coverage. SQLite holds worker state and the restart generation. Manual allocations take precedence over schedules. Among active schedules, lower priority numbers win and list order resolves ties. The controller waits for the previous process to exit before launching a replacement for a changed allocation. Failed workers retry and record their status in the system log.

The supervisor stays up during a receiver or service restart. SQLite transactions protect committed receptions and reporting receipts across worker restarts. ZIP snapshots contain a manifest, a SQLite snapshot and gallery images. Restore validates the manifest and database before copying station data. The operator keeps the TOML configuration separately.

## Storage and delivery

Spots store receiver, mode, observed UTC, RF frequency, message and available callsign and grid. The transaction also records eligibility and optional measurements for live receptions. Public reporters select recent live receptions and persist acknowledgements or UDP send receipts. The local DX Cluster reads newly committed live receptions and bounds each client's output queue. RBN uses the direct `rx/6` API, including handshake configuration, filtering, cadence, quota, endpoint failover and optional confirmation feedback.

See [README.md](README.md) for build and operation. Tests use saved and synthesized signals, an emulated RTL-TCP receiver, and local HTTP and UDP endpoints.
