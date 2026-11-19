#pragma once
#include "receiver/receiver.h"
namespace skim {
// Emit the backend's device parameter, a display label and an optional rate.
using ReceiverCandidate =
    std::function<bool(const std::string&, const std::string&, uint32_t)>;
void ProbeDevices(const std::string& selector, const ReceiverCandidate& found);
void ReadWavIq(const char* path, int rate, const Samples& consume);
void ReadSoapy(const char* device, int64_t center, int rate, double gain,
               double ppm, const Samples& samples,
               const std::function<void()>& gap);
void ReadRtlTcp(const std::string& address, int64_t center, int rate,
                double gain, double ppm, const Samples& consume);
#ifdef SKIM_ALSA
void ReadAlsa(const std::string& name, int rate, const Samples& consume,
              const std::function<void()>& gap);
#endif
}  // namespace skim
