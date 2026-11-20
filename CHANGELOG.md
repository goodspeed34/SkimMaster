# Changelog

All notable changes to Skim Master are recorded here. Versions follow
[Semantic Versioning](https://semver.org/); the receiver and decoder plugin
ABIs are versioned separately by their exported symbol names.

## [Unreleased]

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
