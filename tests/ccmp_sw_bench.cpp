// CPU ceiling for issue #425.  This calls the exact fresh-EVP-context CCMP
// primitive used by ap_wpa2, without USB or airtime in the measurement.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <string>
#include <vector>

#include "ccmp_software.h"

using Clock = std::chrono::steady_clock;

static bool roundtrip(size_t size) {
  uint8_t key[16], nonce[13], aad[22], tag[8];
  for (unsigned i = 0; i < sizeof key; ++i) key[i] = static_cast<uint8_t>(i);
  for (unsigned i = 0; i < sizeof nonce; ++i) nonce[i] = static_cast<uint8_t>(0x20 + i);
  for (unsigned i = 0; i < sizeof aad; ++i) aad[i] = static_cast<uint8_t>(0x40 + i);
  std::vector<uint8_t> plain(size), cipher(size), opened(size);
  for (size_t i = 0; i < size; ++i) plain[i] = static_cast<uint8_t>(i * 29 + 7);
  if (!devourer::test::ccmp_software(true, key, nonce, aad, sizeof aad,
                                      plain.data(), plain.size(), cipher.data(), tag))
    return false;
  uint8_t saved_tag[8];
  std::memcpy(saved_tag, tag, sizeof tag);
  if (!devourer::test::ccmp_software(false, key, nonce, aad, sizeof aad,
                                      cipher.data(), cipher.size(), opened.data(), tag) ||
      opened != plain)
    return false;
  saved_tag[0] ^= 1;
  return !devourer::test::ccmp_software(false, key, nonce, aad, sizeof aad,
                                        cipher.data(), cipher.size(), opened.data(), saved_tag);
}

struct Result {
  uint64_t iterations;
  double wall_seconds;
  double cpu_seconds;
  double wall_ns_per_frame;
  double cpu_ns_per_frame;
  double cpu_mbps;
};

static Result run(bool encrypt, size_t size, int millis) {
  uint8_t key[16] = {}, nonce[13] = {}, aad[22] = {}, tag[8] = {};
  std::vector<uint8_t> plain(size, 0x5a), cipher(size), output(size);
  if (!devourer::test::ccmp_software(true, key, nonce, aad, sizeof aad,
                                     plain.data(), plain.size(), cipher.data(), tag))
    std::abort();
  uint8_t decrypt_tag[8];
  std::memcpy(decrypt_tag, tag, sizeof tag);

  uint64_t iterations = 0;
  const auto start = Clock::now();
  const std::clock_t cpu_start = std::clock();
  const auto deadline = start + std::chrono::milliseconds(millis);
  do {
    // Change the PN-sized tail for seals, as the AP does.  Opens intentionally
    // repeat one valid frame: the CCM work is identical and the tag must stay
    // paired with its nonce and ciphertext.
    if (encrypt) {
      uint64_t pn = iterations + 1;
      for (int i = 0; i < 6; ++i) nonce[7 + i] = static_cast<uint8_t>(pn >> (8 * (5 - i)));
      if (!devourer::test::ccmp_software(true, key, nonce, aad, sizeof aad,
                                         plain.data(), plain.size(), output.data(), tag))
        std::abort();
    } else {
      uint8_t t[8]; std::memcpy(t, decrypt_tag, sizeof t);
      if (!devourer::test::ccmp_software(false, key, nonce, aad, sizeof aad,
                                         cipher.data(), cipher.size(), output.data(), t))
        std::abort();
    }
    ++iterations;
  } while ((iterations & 0xff) != 0 || Clock::now() < deadline);
  const double wall_seconds = std::chrono::duration<double>(Clock::now() - start).count();
  const double cpu_seconds = static_cast<double>(std::clock() - cpu_start) / CLOCKS_PER_SEC;
  return {iterations, wall_seconds, cpu_seconds,
          wall_seconds * 1e9 / iterations, cpu_seconds * 1e9 / iterations,
          iterations * size * 8.0 / cpu_seconds / 1e6};
}

int main(int argc, char** argv) {
  int millis = 400;
  double radio_mbps = 44.55;
  std::vector<size_t> sizes = {64, 256, 512, 1024, 1500};
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--self-test") return roundtrip(1500) ? 0 : 1;
    if (arg == "--millis" && i + 1 < argc) millis = std::max(10, std::atoi(argv[++i]));
    else if (arg == "--radio-mbps" && i + 1 < argc) radio_mbps = std::atof(argv[++i]);
    else if (arg == "--sizes" && i + 1 < argc) {
      sizes.clear();
      std::string list = argv[++i];
      size_t pos = 0;
      while (pos < list.size()) {
        size_t end = list.find(',', pos);
        sizes.push_back(std::stoul(list.substr(pos, end - pos)));
        if (end == std::string::npos) break;
        pos = end + 1;
      }
    }
  }
  if (!roundtrip(1500)) {
    std::fprintf(stderr, "CCMP roundtrip/tag-rejection self-test failed\n");
    return 1;
  }
  std::printf("# CCMP software ceiling; fresh EVP context per frame; OpenSSL %s\n",
              OpenSSL_version(OPENSSL_VERSION));
  std::printf("# projected core %% is at %.2f payload Mbit/s per direction\n", radio_mbps);
  std::printf("# payload  operation  cpu-ns/fr  wall-ns/fr  CPU-payload-Mbit/s  core%%\n");
  for (size_t size : sizes) {
    for (bool encrypt : {true, false}) {
      Result r = run(encrypt, size, millis);
      const double core_pct = radio_mbps / r.cpu_mbps * 100.0;
      std::printf("%8zu  %-9s %9.1f  %10.1f  %18.2f  %5.2f\n", size,
                  encrypt ? "encrypt" : "decrypt", r.cpu_ns_per_frame,
                  r.wall_ns_per_frame, r.cpu_mbps, core_pct);
      std::printf("{\"ev\":\"ccmp.sw_bench\",\"bytes\":%zu,\"operation\":\"%s\","
                  "\"iterations\":%llu,\"wall_seconds\":%.6f,\"cpu_seconds\":%.6f,"
                  "\"wall_ns_per_frame\":%.1f,\"cpu_ns_per_frame\":%.1f,"
                  "\"cpu_payload_mbps\":%.3f,\"radio_payload_mbps\":%.3f,"
                  "\"projected_core_pct\":%.3f}\n", size,
                  encrypt ? "encrypt" : "decrypt",
                  static_cast<unsigned long long>(r.iterations), r.wall_seconds,
                  r.cpu_seconds, r.wall_ns_per_frame, r.cpu_ns_per_frame,
                  r.cpu_mbps, radio_mbps, core_pct);
    }
  }
  return 0;
}
