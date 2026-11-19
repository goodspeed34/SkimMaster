#include "core/clock_health.h"
#ifdef __linux__
#include <sys/timex.h>
#elif defined(_WIN32)
#include <algorithm>
#include <array>
#include <boost/asio.hpp>
#include <cstdlib>
#include <thread>
#endif
#include <chrono>
#include <mutex>
namespace skim {
bool ClockSynchronized() {
  static std::mutex lock;
  static std::chrono::steady_clock::time_point checked{};
  static bool synchronized = false;
  auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> guard(lock);
  if (now - checked < std::chrono::seconds(30)) return synchronized;
  checked = now;
#ifdef __linux__
  timex clock{};
  int state = adjtimex(&clock);
  synchronized = state >= 0 && state != TIME_ERROR &&
                 !(clock.status & STA_UNSYNC) && clock.maxerror < 500000;
#elif defined(_WIN32)
  // SNTP timestamps carry UTC seconds since 1900 and a 32-bit fraction.
  // Verify both the echoed transmit timestamp and the measured clock offset.
  using Udp = boost::asio::ip::udp;
  auto epoch = [] {
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  };
  auto get = [](const unsigned char* p) {
    uint64_t n = 0;
    for (int i = 0; i < 8; ++i) n = (n << 8) | p[i];
    return n;
  };
  synchronized = false;
  try {
    boost::asio::io_context io;
    Udp::resolver resolver(io);
    Udp::socket sock(io, Udp::v4());
    const char* configured = std::getenv("SKIM_NTP_SERVER");
    auto endpoint =
        resolver
            .resolve(Udp::v4(), configured ? configured : "time.windows.com",
                     "123")
            .begin()
            ->endpoint();
    sock.connect(endpoint);
    sock.non_blocking(true);
    std::array<unsigned char, 48> request{}, reply{};
    request[0] = 0x23;
    auto t1 = epoch();
    auto stamp = static_cast<uint64_t>((t1 + 2208988800.0) * 4294967296.0);
    for (int i = 0; i < 8; ++i)
      request[47 - i] = static_cast<unsigned char>(stamp >> (8 * i));
    sock.send(boost::asio::buffer(request));
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline) {
      boost::system::error_code ec;
      auto n = sock.receive(boost::asio::buffer(reply), 0, ec);
      if (ec == boost::asio::error::would_block) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      if (ec || n < 48 || (reply[0] & 7) != 4 || (reply[0] >> 6) == 3 ||
          reply[1] == 0 || reply[1] > 15 || get(reply.data() + 24) != stamp)
        break;
      auto t4 = epoch(),
           t2 = get(reply.data() + 32) / 4294967296. - 2208988800.,
           t3 = get(reply.data() + 40) / 4294967296. - 2208988800.;
      synchronized =
          std::abs(((t2 - t1) + (t3 - t4)) / 2) < .25 && t4 - t1 < .5;
      break;
    }
  } catch (const std::exception&) {
    synchronized = false;
  }
#endif
  return synchronized;
}
}  // namespace skim
