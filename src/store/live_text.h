#pragma once

#include <sqlite3.h>

#include <memory>
#include <string>
#include <vector>

#include "core/types.h"

namespace skim {
// Publishes the recent text of one CW or RTTY slice to the live_text table.
// Rows for the slice are replaced on each update and removed on destruction.
class SmLiveTextWriter {
 public:
  SmLiveTextWriter(const char* database, std::string slice_id,
                   std::string receiver, std::string mode);
  ~SmLiveTextWriter();
  SmLiveTextWriter(const SmLiveTextWriter&) = delete;
  SmLiveTextWriter& operator=(const SmLiveTextWriter&) = delete;

  // channels carry RF frequencies.
  void Replace(int64_t utc_ms, const std::vector<SmTextChannel>& channels);

 private:
  void Clear();

  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db_{nullptr,
                                                         sqlite3_close};
  const std::string slice_id_, receiver_, mode_;
};
}  // namespace skim
