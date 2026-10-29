// SPDX-License-Identifier: GPL-2.0-or-later
// p2plink reporting layer: publishes StatsSnapshot to a POSIX shm object
// (lock-free seqlock, any reader) and as one NDJSON line on stdout
// ({"ev":"p2p.stats",...}) so apps can stay within the budget it announces.
#pragma once
#include <cstdint>
#include <string>
#include "p2p/report.h"

namespace p2p {

class StatsPublisher {
 public:
  ~StatsPublisher();
  // shm_name e.g. "/p2plink.tx"; empty = shm disabled. ndjson = also print.
  bool open(const std::string& shm_name, bool ndjson, std::string* err);
  void publish(const StatsSnapshot& s);
  // Reader side (any process): returns false if absent or torn.
  static bool read(const std::string& shm_name, StatsSnapshot* out);
  static std::string to_ndjson(const StatsSnapshot& s);

 private:
  uint8_t* map_ = nullptr;
  size_t map_len_ = 0;
  bool ndjson_ = false;
  std::string name_;
};

}  // namespace p2p
