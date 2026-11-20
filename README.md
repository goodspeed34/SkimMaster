# Skim Master

Skim Master takes wideband IQ from one or more receivers, allocates decoder slices, and stores and publishes spots. The local control panel provides receiver and decoder allocation status, spectrum and waterfall, heard stations and grids, activity, SSTV images, setup, and station settings.

## Build and validate

Install Meson 1.3 or later, Ninja, Python 3.10 or later, a C++17 compiler, Boost with Boost.Process v2, NNG, FFTW3 single precision, SQLite, Inja with nlohmann/json, libsndfile, libpng, libzip, OpenSSL, SoapySDR, and PortAudio. Meson can fetch the pinned libcsdr, libmorsedec, FT8/FT4, WSPR, toml++ and cpp-httplib sources when needed.

```sh
meson setup build
meson compile -C build -j 2
meson test -C build --print-errorlogs
```

Keep `SkimMasterTools` and the receiver and decoder shared modules beside `SkimMaster`. Meson install places them there. Linux and MSYS2 clang64 are the targets.

## Run a station

Start the server from the directory where you want its configuration and data. It loads `config.toml` from that directory and writes an embedded sample there on first launch. The sample listens on `127.0.0.1:8080`. Open it in your local browser to run the Setup Wizard.

Pass an explicit TOML path to use an existing configuration elsewhere.

```sh
./build/SkimMaster
./build/SkimMaster station.toml
```

```toml
listen_address = "127.0.0.1"
port = 8080
first_run_wizard = true
database = "station.sqlite"
gallery = "gallery"

[station]
callsign = "W1XYZ"
email = "operator@example.com"
name = "Station Operator"
grid = "FN42"

[[receivers]]
receiver = "hf"
device = "driver=airspyhf"
rate = 192000
center_hz = 14075000

[[receivers.slices]]
mode = "FT8"
dial_hz = 14074000

[[receivers.slices]]
mode = "FT4"
dial_hz = 14080000

[[receivers.slices]]
mode = "CW"
```

The server prints startup, listener and worker lifecycle messages to stderr. Open the configured address and port. The Setup Wizard opens on the first visit when `first_run_wizard` is true and the receiver list is empty. The wizard saves the callsign, email, name and grid under `[station]` and adds the IQ source under `[[receivers]]`. Callsign and grid are required for station identity. Email and name are optional. Finishing setup clears the first-run flag. The Station Settings page edits the same TOML file. Receiver, schedule and service changes reload during operation. Listener and data path changes apply after **Restart station** on the Restart Daemon page, or after restarting the server. Relative data paths resolve beside the TOML file.

The panel has no built-in login. Anyone who can reach the configured HTTP listener can change station settings or download a backup. The default listener is loopback. Legacy `user` and `password` keys are ignored and dropped when the configuration is next saved.

Use a capture rate supported by the source. Skim Master resamples to the slice rate when the ratio is fractional. CW uses 48 kHz slices. The other modes use 12 kHz slices. Timed modes center their slice 1,500 Hz above the USB dial. The device string selects a SoapySDR driver, `rtltcp:host:port`, or `portaudio:device-index`. PortAudio captures stereo zero-IF IQ with I on the left and Q on the right. Set RF tuning and gain on the hardware. SoapySDR and RTL-TCP accept optional `gain_db` and `ppm`. Linux checks the kernel clock state before decoding live timed slots. Windows uses SNTP with the server set by `SKIM_NTP_SERVER`, defaulting to `time.windows.com`.

CW and RTTY search the configured slice for signals. By default, the slice inherits the IQ source's RF center. Use an optional `center_hz` inside the slice to offset it within the capture bandwidth. Existing CW and RTTY configurations with `dial_hz` use that value as the slice center when `center_hz` is omitted.

### Decoder settings

The CW, RTTY and digital decoder pages edit per-mode settings. Each page saves a `[decoders.<MODE>]` table, keeping only values that differ from the defaults. A slice can override them with its own `settings` table. Saving restarts only the receiver workers that use the mode.

| Mode | Settings |
|---|---|
| CW | `low_hz`, `high_hz` (search edges relative to the slice center), `max_channels`, `initial_wpm` |
| RTTY | `shift_hz` (85, 170, 425 or 850), `baud` (45.45, 50 or 75), `reverse`, `max_channels`, `threshold` |
| FT8, FT4 | `min_hz`, `max_hz` (audio search edges above the dial), `max_candidates`, `min_score`, `ldpc_iterations` |
| WSPR | `passes` (1–3), `subtraction` |

```toml
[decoders.RTTY]
shift_hz = 850
reverse = true

[[receivers.slices]]
mode = "CW"
settings = { max_channels = 8 }
```

Unknown keys and out-of-range values are rejected when the TOML is saved.

The CW and RTTY pages also show **Live Channel Text**: the last 160 characters each tracked channel has decoded, with its RF, speed and SNR. Receiver workers replace this text about once a second in the transient `live_text` table and remove it when they stop. The page polls it every two seconds unless refresh is paused.

**Decode a Recording** on each decoder page decodes an uploaded WAV with the saved decoder settings and shows the results without storing or reporting them. FT8, FT4 and WSPR take one complete slot of 12 kHz mono audio and its USB dial. CW takes 48 kHz and RTTY 12 kHz stereo IQ, with I on the left, Q on the right and the RF center. Uploads are limited to 64 MiB, and one decode runs at a time.

A receiver with a top-level `slices` array reserves its tuning for manual allocation. **Band Schedules** adds, edits, duplicates and deletes schedules. It also runs a schedule immediately for 30 minutes with **Run Now**. **Clear Override** restores normal allocation immediately. Automatic schedules can run on receivers that have no manual allocation. Each schedule has `start_minute`, `end_minute`, `priority` and `center_hz`, with optional `name`, `enabled`, `days` and `slices` fields. Minutes count from UTC midnight. Equal start and end cover the full day. An overnight window continues into the day after its start. The `days` mask uses bit 0 for Monday through bit 6 for Sunday and defaults to all seven days. Schedules are enabled by default. Lower priority numbers win, with ties resolved by list order.

## Manage IQ sources

**Add / Edit Receiver** lists configured sources with **Edit** and **Delete** actions. **Add Receiver** can create an unallocated source or add its first decoder slice. To attach another slice, edit the source, choose **Attach Decoder Slice**, then save. Use **Edit** next to an existing slice to change its mode or tuning or to delete it. The receiver editor lists manual, attached and schedule-specific slices. Each slice activates when its full passband fits the tuned IQ bandwidth. Out-of-band slices stay attached for later tuning. For scheduled receivers, the editor stores additional receiver slices in `receivers.attached_slices` so each schedule evaluates them alongside its own slices. **Test Configuration** attempts a short IQ capture using the entered device and tuner settings. Stop an active hardware allocation before testing its device. Deleting a source removes its configuration and allocations and keeps its recorded receptions. **Receiver Status** displays each source's RF center, rate, gain, active slices, last IQ timestamp and worker state.

In the receiver editor or setup wizard, select the driver and click **Scan**. A discovery result fills in the device parameter and the sample rate when available. The device parameter is also editable by hand.

| Driver | Discovery target | Manual parameter example |
|---|---|---|
| SoapySDR | Leave blank to enumerate, or supply a SoapySDR argument filter | `driver=airspyhf` |
| RTL-TCP | Enter `host:port` to check its RTL-TCP handshake. A blank target checks `127.0.0.1:1234`. | `rtltcp:192.168.1.20:1234` |
| TCI | Enter `host[:port]` of a TCI server such as ExpertSDR. The default port is 40001. A blank target checks `127.0.0.1:40001`. Each transceiver receiver is listed separately. | `tci:192.168.1.30:40001/0` |
| PortAudio | Leave blank to list stereo inputs, or filter by device name or index | `portaudio:2` or `portaudio:default` |
| IQ WAV | Enter a server-side file or directory. A blank target scans the TOML directory. | `wav:recordings/20m.wav` |

TCI capture sets the server's IQ rate (48000, 96000, 192000 or 384000 Hz) and the receiver's DDS center, then streams that receiver's IQ. Set gain and calibration in the radio software. TCI servers share one DDS per receiver with every connected client. If another client retunes it, Skim Master logs the change and holds that receiver's IQ until the DDS returns to the configured center.

PortAudio device indices can change as audio devices are added or removed. Rescan if the configured index no longer selects the intended stereo input. When upgrading an ALSA configuration, replace any `alsa:` selectors with a scanned PortAudio input. If reusing an install directory, remove the former bundled `librcvralsa` module before restarting.

The SSTV Gallery lists the 100 newest images. Filter by mode and UTC date, download a CSV index of matching images, or delete an individual image. The timestamp and mode come from the image filename; Skim Master does not attach callsign or signal metadata to gallery files.

## Play a recorded IQ WAV

In Setup Wizard, enter `wav:PATH` as the Device and fill in the capture rate and receiver RF center. Fill in a dial frequency if the selected decoder asks for one. Use a WAV file on the server. The same settings are available in TOML. A relative path resolves beside the station TOML. Paths beginning with `~/` resolve against the server account’s home directory. The recording must be stereo with I on the left and Q on the right. Set `rate` to the WAV sample rate and `center_hz` to the RF center used when recording.

```toml
[[receivers]]
receiver = "recorded-20m"
device = "wav:recordings/20m.wav"
rate = 48000
center_hz = 14075500
repeat = true

[[receivers.slices]]
mode = "FT8"
dial_hz = 14074000
```

The wizard selects **Repeat WAV playback** by default. Set `repeat = true` in TOML to loop the recording at its original sample rate, with decoder state reset at each wrap. With repeat disabled, playback runs once at processing speed. To replay a one-shot recording, restart the receiver workers or change the allocation. Set `utc_ms` to the Unix UTC timestamp in milliseconds of the first IQ frame when known. With the default of zero, timed decoders treat the first frame as a slot boundary. Decoded results remain in the panel and database. All reporting feeds, including the local Telnet DX Cluster, use live receptions.

## Spectrum tuning

**Spectrum / Waterfall** shades the passband of each running slice. Click the trace to add a slice there: CW and RTTY center their slice on the clicked frequency, and FT8, FT4 and WSPR set the USB dial 1500 Hz below it so the signal falls mid-passband. Adjust the frequency or mode before saving. The slice is added to the receiver's manual slices, or to its schedule-attached slices if it runs on schedules, and the receiver restarts to apply it.

## Band and mode activity

**Band & Mode Activity** counts stored decodes by amateur band (from the decoded RF frequency) and mode over the last hour, day, week or 30 days. It shows a band × mode matrix with heat shading, absolute counts or per-band or per-mode percentages, an hourly UTC heatmap per band, and mode totals with share, unique calls and peak hour. Filter by receiver and mode, and export the matrix as CSV.

## Noise floor

Each running receiver records its noise floor once a minute as the median power of its unoccupied FFT bins, excluding carriers and the band edges. The value is a density in dBFS/Hz, so receivers with different sample rates compare directly; add your receiver's calibration to read dBm/Hz. **Noise Floor Analysis** charts the last 24 hours per receiver with optional smoothing, lists per-band minimum, average and maximum with a six-hour trend, counts minutes above an optional alert threshold, and exports the minute values as CSV. System Summary shows each receiver's latest value.

## Reporting and backups

The **DX Cluster**, **PSK Reporter**, and **RBN** pages configure their services; WSPRnet settings share the PSK Reporter page. Each service is disabled by default. The forms expose the options used by the running workers, station callsign and grid fallback, and current worker and receipt state. Saving a service change reloads its worker. An enabled service can be restarted locally without restarting other services or receiver workers. Blank host, port, or endpoint fields use the service default.

The equivalent `[services.cluster]`, `[services.psk]`, `[services.wsprnet]` and `[services.rbn]` TOML tables remain available for advanced settings. Each enabled service uses the `[station]` callsign and grid unless the service supplies an override. RBN uses the station email as the default `sign_in` and takes `band_limits` (comma-separated kHz ranges). It talks directly to the RBN `rx/6` HTTP API and follows the server's spot windows, upload cadence and quota. The local DX Cluster listens on loopback. Receipt rows record the service's send path; PSK Reporter uses UDP and does not provide a remote acknowledgment. Only receptions from live receiver streams enter public reporting. Keep reporting disabled for simulated network receivers.

```sh
./build/SkimMasterTools --backup station.sqlite gallery snapshot.zip
./build/SkimMasterTools --restore snapshot.zip station.sqlite gallery
```

**Backup & Restore** downloads a snapshot, restores an uploaded one and downloads a diagnostics bundle for bug reports, with email, operator name and RBN sign-in redacted. A panel restore validates the upload, then restarts the station to apply it. Offline restore with `SkimMasterTools --restore` requires the station to be stopped. Either way, restore validates the archive before changing the database and takes a pre-restore snapshot beside it. **Factory reset** requires typing RESET, saves a snapshot, then clears the database and SSTV images; it replaces the TOML with the first-run sample only when selected. Keep the station TOML separately from the database and gallery archive.

## Replay

`SkimMasterTools --replay IQ_FILE` accepts whitespace-separated `sample_index I Q` triples at 48 kHz. Defaults are 14.070 MHz receiver center and 14.076 MHz slice center. Add `RECEIVER_HZ SLICE_HZ` to change them. Gaps reset the slicer and tone decoder.

`--decode FT8|FT4|WSPR AUDIO DIAL_HZ UTC_MS RECEIVER DB` decodes a complete slot from 12 kHz mono audio. `--receive` and `--sstv` also accept interleaved native float32 IQ pairs on standard input using device `-`. Run `SkimMasterTools --help` for command syntax.


## Building an external plugin

Meson installs the C++17 plugin SDK under `include/skimmaster`. Add that directory to your plugin's include path and build a shared module with the same C++ toolchain as the host. A receiver includes `receiver/plugin_api.h`, exports `skim_receiver_v3` and `skim_receiver_descriptor_v3`, and uses a filename starting with `librcvr`. `Capture` receives an `SmCaptureSink` with sample, gap and state callbacks. A plugin that can read back what the device applied sets `reports_state` and calls `state` at start and whenever the tuning changes. The host logs the reported center, rate and gain, and holds IQ while they differ from the request. A device with several IQ streams lists each as its own device string from `Probe`. A slot decoder includes `decoder/api.h` and can use the worker in `decoder/slot_plugin.h` with NNG and nlohmann-json. It exports `skim_slot_decoder_v3` and uses a filename starting with `libdecoder`. Its metadata lists the settings the operator can change; the host validates `[decoders.<MODE>]` against them, shows them on the decoder page and passes the resolved values to `start` and the decode callback.

Place the module beside the SkimMaster executables or under their `plugins/` directory. Use `.so` on Linux and `.dll` on Windows. Restart the station to discover newly installed modules. The standalone example under `tests/plugin_sdk/` builds a receiver and a decoder from the installed headers and exercises both through `SkimMasterTools`.

## License

Skim Master is licensed under the GNU Affero General Public License, version 3 or any later version (SPDX `AGPL-3.0-or-later`). See [COPYING](COPYING) and [third-party notices](THIRD-PARTY-NOTICES.txt).

The control panel, setup wizard, and DX Cluster greeting offer the corresponding source. Git builds default to the checkout's commit on the upstream repository. When deploying modified builds or building from a source archive, set the Meson option `-Dsource_url=https://your-host.example/skimmaster-source` to the publicly accessible corresponding source for the deployed build. Include dependency source and build scripts as required by their licenses.
