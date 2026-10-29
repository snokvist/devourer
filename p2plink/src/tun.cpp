// SPDX-License-Identifier: GPL-2.0-or-later
#include "p2p/tun.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace p2p {

Tun::~Tun() { if (fd_ >= 0) close(fd_); }

bool Tun::open(const std::string& name, int mtu, std::string* err) {
  fd_ = ::open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd_ < 0) { if (err) *err = std::string("open /dev/net/tun: ") + strerror(errno); return false; }
  ifreq ifr{};
  ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
  std::strncpy(ifr.ifr_name, name.c_str(), IFNAMSIZ - 1);
  if (ioctl(fd_, TUNSETIFF, &ifr) < 0) { if (err) *err = std::string("TUNSETIFF: ") + strerror(errno); close(fd_); fd_ = -1; return false; }
  name_ = ifr.ifr_name;
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s >= 0) {
    ifreq m{};
    std::strncpy(m.ifr_name, name_.c_str(), IFNAMSIZ - 1);
    m.ifr_mtu = mtu;
    if (ioctl(s, SIOCSIFMTU, &m) < 0 && err) *err = std::string("SIOCSIFMTU: ") + strerror(errno);
    close(s);
  }
  return true;
}

long Tun::read_packet(uint8_t* buf, size_t cap) {
  ssize_t n = ::read(fd_, buf, cap);
  if (n > 0) return n;
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
  return -1;
}

bool Tun::write_packet(const uint8_t* pkt, size_t len) { return ::write(fd_, pkt, len) == static_cast<ssize_t>(len); }

bool Tun::set_ipv4(const std::string& cidr, std::string* err) {
  auto slash = cidr.find('/');
  std::string addr = cidr.substr(0, slash);
  int prefix = slash == std::string::npos ? 32 : std::stoi(cidr.substr(slash + 1));
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) { if (err) *err = strerror(errno); return false; }
  ifreq ifr{};
  std::strncpy(ifr.ifr_name, name_.c_str(), IFNAMSIZ - 1);
  auto* sin = reinterpret_cast<sockaddr_in*>(&ifr.ifr_addr);
  sin->sin_family = AF_INET;
  if (inet_pton(AF_INET, addr.c_str(), &sin->sin_addr) != 1) { close(s); if (err) *err = "bad address"; return false; }
  if (ioctl(s, SIOCSIFADDR, &ifr) < 0) { if (err) *err = std::string("SIOCSIFADDR: ") + strerror(errno); close(s); return false; }
  sin->sin_addr.s_addr = htonl(prefix == 0 ? 0 : (0xffffffffu << (32 - prefix)));
  if (ioctl(s, SIOCSIFNETMASK, &ifr) < 0) { if (err) *err = std::string("SIOCSIFNETMASK: ") + strerror(errno); close(s); return false; }
  close(s);
  return true;
}

bool Tun::set_up(bool up, std::string* err) {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) { if (err) *err = strerror(errno); return false; }
  ifreq ifr{};
  std::strncpy(ifr.ifr_name, name_.c_str(), IFNAMSIZ - 1);
  if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) { if (err) *err = strerror(errno); close(s); return false; }
  if (up) ifr.ifr_flags |= IFF_UP | IFF_RUNNING; else ifr.ifr_flags &= ~IFF_UP;
  bool ok = ioctl(s, SIOCSIFFLAGS, &ifr) == 0;
  if (!ok && err) *err = std::string("SIOCSIFFLAGS: ") + strerror(errno);
  close(s);
  return ok;
}

uint8_t classify_ip(const uint8_t* p, size_t len) {
  if (len < 20) return 2;
  const uint8_t ver = p[0] >> 4;
  uint8_t dscp = 0, proto = 0;
  if (ver == 4) { dscp = p[1] >> 2; proto = p[9]; }
  else if (ver == 6 && len >= 40) { dscp = uint8_t(((p[0] & 0x0f) << 2) | (p[1] >> 6)); proto = p[6]; }
  else return 2;
  if (dscp >= 48) return 0;                       // CS6/CS7: network control
  if (proto == 1 || proto == 58) return 0;        // ICMP / ICMPv6: keep ping honest
  if (dscp == 46 || (dscp >= 32 && dscp <= 38)) return 1;  // EF, AF4x
  if (proto == 17 && len <= 256) return 1;        // small UDP: RC / telemetry
  return 2;
}

}  // namespace p2p
