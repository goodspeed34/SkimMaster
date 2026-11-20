# SDR Console Data Repeater: spike result

**Decision (2026-11-20): not implemented.** Skim Master has no `sdrconsole:` receiver module. Revisit if SDR-Radio.com publishes a wire-level specification or a headless, cross-platform client library.

## What is published

The [Data Repeater page](https://www.sdr-radio.com/data-repeater) describes:

- a TCP connection to port 50050 (configurable), answered with a `REPEATER_SETTINGS` structure;
- a client `ACK` every second, answered with `REPEATER_ACK`;
- 16-bit integer or 32-bit float IQ, with the radio name, center frequency and bandwidth;
- a Windows DLL, `SDRRepeater.dll`, whose connection call opens a modal MFC dialog (`SDRRepeaterConnectDlg(CWnd*)`) and delivers IQ through a callback.

## What is missing

The page gives no field layout, sizes, packing or byte order for either structure, no message IDs, no IQ framing or interleaving, and no keep-alive timeout. Vendor staff on the [SDR-Radio user group](https://sdr-radio.groups.io/g/main/topic/raw_iq_data_streaming_to/101679839) described the repeater as TCP control with UDP data and said third parties cannot replicate it.

## Why not build it anyway

- A module written from packet captures would break silently whenever SDR Console changes, and Skim Master could not test it against a documented contract or an emulator built from one.
- The DLL is Windows-only. Its connect call needs a parent window and a user's click, so a supervised headless worker cannot use it, and Linux builds could never ship it.

## What SDR Console users can do now

Record IQ in SDR Console and play the file back with the `wav:` receiver, or connect the radio through a source Skim Master supports directly (SoapySDR, RTL-TCP, TCI or FlexRadio).
