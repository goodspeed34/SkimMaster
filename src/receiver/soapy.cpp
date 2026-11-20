#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.hpp>
#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>

#include "receiver/adapters.h"
namespace skim {
void ProbeDevices(const std::string& selector,
                  const SmReceiverCandidate& found) {
  for (const auto& args : SoapySDR::Device::enumerate(selector)) {
    auto device = SoapySDR::KwargsToString(args);
    auto label = args.find("label");
    if (!device.empty() &&
        !found(device, label == args.end() ? device : label->second, 0))
      break;
  }
}
void ReadSoapy(const char* device, int64_t dial_hz, int rate, double gain,
               double ppm, const Samples& consume,
               const std::function<void()>& gap) {
  std::array<std::complex<float>, 4096> samples;
  std::unique_ptr<SoapySDR::Device, void (*)(SoapySDR::Device*)> radio(
      SoapySDR::Device::make(std::string(device)), SoapySDR::Device::unmake);
  if (!radio) throw std::runtime_error("cannot open SoapySDR device");
  radio->setSampleRate(SOAPY_SDR_RX, 0, rate);
  radio->setFrequency(SOAPY_SDR_RX, 0, static_cast<double>(dial_hz));
  if (std::abs(radio->getSampleRate(SOAPY_SDR_RX, 0) - rate) > 0.5)
    throw std::runtime_error("device selected a different sample rate");
  if (std::isfinite(gain)) radio->setGain(SOAPY_SDR_RX, 0, gain);
  if (std::isfinite(ppm) && ppm != 0) {
    if (!radio->hasFrequencyCorrection(SOAPY_SDR_RX, 0))
      throw std::runtime_error("receiver does not support PPM correction");
    radio->setFrequencyCorrection(SOAPY_SDR_RX, 0, ppm);
  }
  auto stream = radio->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32);
  auto close = [&](SoapySDR::Stream* s) { radio->closeStream(s); };
  std::unique_ptr<SoapySDR::Stream, decltype(close)> guard(stream, close);
  if (radio->activateStream(stream) < 0)
    throw std::runtime_error("receiver activation failed");
  void* buffers[] = {samples.data()};
  bool timed = false;
  long long expected = 0;
  for (;;) {
    int flags = 0;
    long long time = 0;
    int n =
        radio->readStream(stream, buffers, samples.size(), flags, time, 250000);
    if (n == SOAPY_SDR_TIMEOUT) continue;
    if (n == SOAPY_SDR_OVERFLOW) {
      gap();
      continue;
    }
    if (n < 0) throw std::runtime_error(SoapySDR::errToStr(n));
    if (flags & SOAPY_SDR_HAS_TIME) {
      if (timed && std::abs(time - expected) > 2000000000LL / rate) gap();
      expected = time + std::llround(n * 1e9 / rate);
      timed = true;
    } else
      timed = false;
    consume(samples.data(), n);
    if (flags & SOAPY_SDR_END_ABRUPT) gap();
    if (flags & SOAPY_SDR_END_BURST) break;
  }
  radio->deactivateStream(stream);
}
}  // namespace skim
