#pragma once
#include <cstdint>
#include <string>

#include "receiver/receiver.h"
namespace skim {
void ReceiveSstv(const char* device, int64_t dial_hz, int rate,
                 const char* gallery, const IqReader& read = ReadIq);
void ReceiveMode(const std::string& mode, const char* device, int64_t dial,
                 const char* receiver, const char* database, int64_t first_ms,
                 const IqReader& read = ReadIq);
}  // namespace skim
