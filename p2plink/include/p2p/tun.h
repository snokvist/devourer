// SPDX-License-Identifier: GPL-2.0-or-later
// p2plink: Linux TUN device (layer-3, no packet info header). The IP tunnel
// that carries everything that is not video. Fixed addressing is the
// caller's job (ip addr add ... dev <name>); this only opens the fd.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace p2p {

class Tun {
 public:
  Tun() = default;
  ~Tun();
  Tun(const Tun&) = delete;
  Tun& operator=(const Tun&) = delete;
  // Opens /dev/net/tun as IFF_TUN|IFF_NO_PI, non-blocking. `name` may be a
  // pattern like "p2p%d"; the realised name is in name(). Sets the MTU.
  bool open(const std::string& name, int mtu, std::string* err);
  int fd() const { return fd_; }
  const std::string& name() const { return name_; }
  // Non-blocking read of one packet; >0 bytes, 0 would-block, -1 error.
  long read_packet(uint8_t* buf, size_t cap);
  bool write_packet(const uint8_t* pkt, size_t len);
  // Best-effort IP configuration via the netlink-free path (ioctl SIOCSIFADDR
  // etc.): "10.99.0.1/30". Returns false (err) when not permitted.
  bool set_ipv4(const std::string& cidr, std::string* err);
  bool set_up(bool up, std::string* err);

 private:
  int fd_ = -1;
  std::string name_;
};

// DSCP-based class for the TX scheduler: 0 control (CS6/CS7, ICMP), 1
// interactive (EF, AF4x, small packets <= 256 B UDP), 2 bulk. Pure.
uint8_t classify_ip(const uint8_t* pkt, size_t len);

}  // namespace p2p
