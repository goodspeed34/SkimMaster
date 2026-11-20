#pragma once
#include <cstdint>
#include <string>

#include "receiver/receiver.h"
namespace skim {
class SmSlice;
struct DecoderRoute;
struct DecoderResult;
void ReceiveSstv(const char* device, int64_t dial_hz, int rate,
                 const char* gallery, const IqReader& read = ReadIq);
void ReceiveSstv(const SmSlice& slice, int64_t dial, int rate,
                 const char* gallery, const IqReader& read);
void ReceiveMode(const std::string& mode, const char* device, int64_t dial,
                 const char* receiver, const char* database, int64_t first_ms,
                 const IqReader& read = ReadIq);
void ReceiveSlice(const SmSlice& slice, int64_t dial, const char* database,
                  int64_t first_ms, const IqReader& read,
                  const DecoderRoute* route = nullptr);
void PublishSlot(DecoderResult result, int64_t dial, const char* receiver,
                 const char* mode, const char* database, bool live);
}  // namespace skim
