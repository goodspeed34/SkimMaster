// A SoapySDR module for tests. driver=fake streams interleaved float32 IQ
// from the file named by the iq argument. step rounds the tuned frequency to
// a synthesizer grid.

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Registry.hpp>
#include <cmath>
#include <complex>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::string Arg(const SoapySDR::Kwargs& args, const char* key,
                const char* fallback = "") {
  const auto found = args.find(key);
  return found == args.end() ? fallback : found->second;
}

class SmFakeDevice final : public SoapySDR::Device {
 public:
  explicit SmFakeDevice(const SoapySDR::Kwargs& args)
      : path_(Arg(args, "iq")), step_(std::stod(Arg(args, "step", "1"))) {}

  std::string getDriverKey() const override { return "fake"; }
  std::string getHardwareKey() const override { return "fake"; }
  size_t getNumChannels(const int) const override { return 1; }

  void setFrequency(const int, const size_t, const double frequency,
                    const SoapySDR::Kwargs&) override {
    frequency_ = std::round(frequency / step_) * step_;
  }
  double getFrequency(const int, const size_t) const override {
    return frequency_;
  }
  void setSampleRate(const int, const size_t, const double rate) override {
    rate_ = rate;
  }
  double getSampleRate(const int, const size_t) const override { return rate_; }
  std::vector<std::string> listGains(const int, const size_t) const override {
    return {"LNA"};
  }
  void setGain(const int, const size_t, const double value) override {
    gain_ = value;
  }
  double getGain(const int, const size_t) const override { return gain_; }
  SoapySDR::Range getGainRange(const int, const size_t) const override {
    return {0, 50};
  }

  SoapySDR::Stream* setupStream(const int, const std::string& format,
                                const std::vector<size_t>&,
                                const SoapySDR::Kwargs&) override {
    if (format != SOAPY_SDR_CF32)
      throw std::runtime_error("fake device streams CF32 only");
    file_.reset(std::fopen(path_.c_str(), "rb"));
    if (!file_) throw std::runtime_error("fake device cannot open " + path_);
    return reinterpret_cast<SoapySDR::Stream*>(this);
  }
  void closeStream(SoapySDR::Stream*) override { file_.reset(); }
  int activateStream(SoapySDR::Stream*, const int, const long long,
                     const size_t) override {
    return 0;
  }
  int deactivateStream(SoapySDR::Stream*, const int, const long long) override {
    return 0;
  }

  int readStream(SoapySDR::Stream*, void* const* buffers, const size_t count,
                 int& flags, long long& time_ns, const long) override {
    flags = 0;
    time_ns = 0;
    auto* out = static_cast<std::complex<float>*>(buffers[0]);
    const auto n = std::fread(out, sizeof(*out), count, file_.get());
    if (n < count || std::feof(file_.get())) flags |= SOAPY_SDR_END_BURST;
    return static_cast<int>(n);
  }

 private:
  struct SmClose {
    void operator()(std::FILE* file) const { std::fclose(file); }
  };
  std::string path_;
  double step_;
  double frequency_ = 0;
  double rate_ = 0;
  double gain_ = 0;
  std::unique_ptr<std::FILE, SmClose> file_;
};

SoapySDR::KwargsList FindFake(const SoapySDR::Kwargs& args) {
  const auto driver = Arg(args, "driver");
  if (!driver.empty() && driver != "fake") return {};
  return {{{"driver", "fake"}, {"label", "Fake SDR"}, {"serial", "F00D"}}};
}

SoapySDR::Device* MakeFake(const SoapySDR::Kwargs& args) {
  return new SmFakeDevice(args);
}

const SoapySDR::Registry kRegistration("fake", &FindFake, &MakeFake,
                                       SOAPY_SDR_ABI_VERSION);
}  // namespace
