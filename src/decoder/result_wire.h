#pragma once

#include <nng/nng.h>

#include <cmath>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include "decoder/api.h"

namespace skim {
namespace decoder_wire {
inline void Check(int rc) {
  if (rc) throw std::runtime_error(nng_strerror(rc));
}

struct Socket {
  explicit Socket(int (*open)(nng_socket*)) { Check(open(&socket)); }
  ~Socket() { nng_close(socket); }
  Socket(const Socket&) = delete;
  nng_socket socket{};
};

using Message = std::unique_ptr<nng_msg, decltype(&nng_msg_free)>;

inline Message Receive(nng_socket socket, bool timeout_ok = false) {
  nng_msg* message = nullptr;
  const int rc = nng_recvmsg(socket, &message, 0);
  if (!timeout_ok || rc != NNG_ETIMEDOUT) Check(rc);
  return Message(message, nng_msg_free);
}

inline void Send(nng_socket socket, const std::string& payload) {
  Check(nng_send(socket, const_cast<char*>(payload.data()), payload.size(), 0));
}

inline std::string Encode(const DecoderResult& result) {
  auto spots = nlohmann::json::array();
  for (const auto& spot : result.spots) {
    nlohmann::json item{{"relative_freq_hz", spot.frequency},
                        {"message", spot.message},
                        {"callsign", spot.callsign},
                        {"grid", spot.grid}};
    if (spot.snr && std::isfinite(*spot.snr)) item["snr"] = *spot.snr;
    if (spot.wspr)
      item["wspr"] = {{"dt", spot.wspr->dt}, {"power", spot.wspr->power}};
    spots.push_back(std::move(item));
  }
  return nlohmann::json{{"decoder_id", result.decoder_id},
                        {"utc_ms", result.utc_ms},
                        {"spots", std::move(spots)}}
      .dump();
}

inline DecoderResult Decode(nng_msg* message) {
  const auto* bytes = static_cast<const char*>(nng_msg_body(message));
  auto data = nlohmann::json::parse(bytes, bytes + nng_msg_len(message));
  DecoderResult result{data.at("decoder_id"), data.at("utc_ms"), {}};
  for (const auto& item : data.at("spots")) {
    Spot spot{item.at("relative_freq_hz"), item.at("message"),
              item.at("callsign"), item.at("grid")};
    if (item.contains("snr")) spot.snr = item.at("snr").get<double>();
    if (item.contains("wspr")) {
      const auto& wspr = item.at("wspr");
      spot.wspr = WsprDetails{0, wspr.at("dt"), wspr.at("power")};
    }
    result.spots.push_back(std::move(spot));
  }
  return result;
}
}  // namespace decoder_wire
}  // namespace skim
