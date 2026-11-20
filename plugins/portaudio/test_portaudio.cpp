#include <portaudio.h>

#include <stdexcept>
#include <vector>

#include "receiver/plugin_api.h"

extern "C" const skim::SmReceiverPlugin* skim_receiver_v2() noexcept;

namespace {
bool initialized = false;
bool stream_open = false;
bool started = false;
bool gap_reported = false;
const PaDeviceInfo devices[] = {
    {2, "Mono mic", 0, 1, 0, 0.01, 0, 0.1, 0, 44100},
    {2, "IQ stereo", 0, 2, 0, 0.01, 0, 0.1, 0, 48000},
};
const PaHostApiInfo api{1, paInDevelopment, "Fixture API", 2, 1, paNoDevice};

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}  // namespace

extern "C" {
PaError Pa_Initialize() {
  Check(!initialized, "fixture initialized twice");
  initialized = true;
  return paNoError;
}

PaError Pa_Terminate() {
  Check(initialized && !stream_open,
        "PortAudio terminated before stream close");
  initialized = false;
  return paNoError;
}

PaDeviceIndex Pa_GetDeviceCount() { return 2; }
PaDeviceIndex Pa_GetDefaultInputDevice() { return 1; }

const PaDeviceInfo* Pa_GetDeviceInfo(PaDeviceIndex index) {
  return index >= 0 && index < 2 ? &devices[index] : nullptr;
}

const PaHostApiInfo* Pa_GetHostApiInfo(PaHostApiIndex) { return &api; }
const char* Pa_GetErrorText(PaError) { return "PortAudio fixture error"; }

PaError Pa_OpenStream(PaStream** stream, const PaStreamParameters* input,
                      const PaStreamParameters* output, double rate,
                      unsigned long, PaStreamFlags, PaStreamCallback* callback,
                      void*) {
  Check(initialized && !stream_open && input->device == 1 &&
            input->channelCount == 2 && input->sampleFormat == paFloat32 &&
            !output && !callback && rate == 48000,
        "expected a blocking stereo input stream at the requested rate");
  stream_open = true;
  *stream = &stream_open;
  return paNoError;
}

PaError Pa_StartStream(PaStream*) {
  Check(stream_open, "stream started before open");
  started = true;
  return paNoError;
}

PaError Pa_ReadStream(PaStream*, void* buffer, unsigned long frames) {
  Check(started && frames > 0, "attempted to read an inactive stream");
  auto* data = static_cast<float*>(buffer);
  for (unsigned long i = 0; i < frames; ++i) {
    data[2 * i] = 0.25f;
    data[2 * i + 1] = -0.5f;
  }
  return paInputOverflowed;
}

PaError Pa_CloseStream(PaStream*) {
  Check(stream_open, "stream closed twice");
  stream_open = started = false;
  return paNoError;
}
}

int main() {
  const auto* receiver = skim_receiver_v2();
  std::vector<std::string> ids;
  receiver->Probe("", [&](const auto& id, const auto& label, uint32_t rate) {
    Check(label == "IQ stereo (Fixture API)" && rate == 48000,
          "unexpected PortAudio probe metadata");
    ids.push_back(id);
    return true;
  });
  Check(ids == std::vector<std::string>{"1"} && !initialized,
        "PortAudio probe must list only stereo inputs");

  struct SmStopFixture {};
  try {
    receiver->Capture(
        {"default", 7074000, 48000, {}, {}},
        [](const std::complex<float>* iq, size_t count) {
          Check(gap_reported && count &&
                    iq[0] == std::complex<float>{0.25f, -0.5f},
                "expected overflow notification before delivering "
                "left-I/right-Q");
          throw SmStopFixture{};
        },
        [] { gap_reported = true; });
    throw std::runtime_error("capture returned without delivering IQ");
  } catch (const SmStopFixture&) {
    Check(!initialized && !stream_open,
          "capture cancellation leaked resources");
  }

  try {
    receiver->Capture({"0", 7074000, 48000, {}, {}}, {}, {});
  } catch (const std::runtime_error& error) {
    Check(std::string_view(error.what()) ==
                  "PortAudio IQ requires a stereo input device" &&
              !initialized,
          "expected mono input to be rejected before opening a stream");
    return 0;
  }
  throw std::runtime_error("accepted a mono input");
}
