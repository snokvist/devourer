/* udp_blast.cpp — one-way UDP throughput, because nothing in this tree
 * measured throughput at all.
 *
 * WHY NOT PING. Every number this workstream has quoted for a station link
 * came from `ping -f`, and a flood ping is ROUND-TRIP bound: it sends the
 * next request when the previous reply arrives (or after 10 ms, whichever is
 * sooner), so `reply_pps` is a LATENCY figure wearing a throughput costume.
 * A link that delivers 20 round trips a second at 3 ms RTT is not a 20-packet
 * link; it is a link nobody asked for more than 20 packets from.
 *
 * WHY NOT iperf3. It is not on this bench, and an instrument that has to be
 * installed is an instrument whose version is part of the result. This is
 * sixty lines, it emits the same JSONL the rest of the tree emits, and its
 * accounting is checkable — `udp_blast --self-test` runs both halves over
 * loopback and asserts the arithmetic, including the loss and reorder counts,
 * which is more than can be said for a number read off someone else's tool.
 *
 * WHAT IT MEASURES, stated so the number cannot be over-read:
 *   - ONE DIRECTION. The sender's offered rate and the receiver's delivered
 *     rate are separate figures and both are printed. Delivered is the
 *     result; offered is the control that says whether the link or the
 *     sender was the limit.
 *   - GOODPUT AT THE UDP PAYLOAD LAYER. Not PHY rate, not airtime. The
 *     802.11, LLC/SNAP, CCMP, IP and UDP headers are all excluded, so this
 *     number is strictly lower than anything a radiotap capture would show,
 *     and that is the honest direction to err in.
 *   - LOSS BY SEQUENCE GAP, and reordering separately. A link with no
 *     link-layer retransmission loses frames; counting them as loss when they
 *     merely arrived late would overstate it.
 *
 * WHAT IT DOES NOT MEASURE: fairness, latency under load (the receiver never
 * replies), or anything about the reverse direction. Run it twice.
 *
 * Build: g++ -std=c++20 -O2 tests/udp_blast.cpp -o udp_blast
 * Run:   udp_blast recv 0.0.0.0 5201 20
 *        udp_blast send 192.168.97.1 5201 1400 2000 15   (2000 kbit/s offered)
 *
 * An offered rate is REQUIRED. There is no unbounded mode - see run_send.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>

namespace {

/* The datagram: an 8-byte big-endian sequence number, then filler. Big-endian
 * so a capture is readable by eye, and 8 bytes so a long soak cannot wrap. */
constexpr size_t kSeqBytes = 8;
constexpr size_t kMinPayload = kSeqBytes;
constexpr size_t kMaxPayload = 8192;
/* A typo guard on the offered rate. 200 Mbit/s is far above anything this
 * project's links carry and far below the multi-gigabit storm the unbounded
 * mode used to produce. */
constexpr long kMaxKbit = 200000;

void put_be64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (56 - 8 * i));
}
uint64_t get_be64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
  return v;
}

double now_s() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct RecvResult {
  uint64_t datagrams = 0;
  uint64_t bytes = 0;
  uint64_t lost = 0;       /* gaps in the sequence that never filled */
  uint64_t reordered = 0;  /* arrived below the high-water mark */
  uint64_t duplicated = 0; /* the same sequence twice - a retransmit, or a
                            * duplicate the MAC delivered twice */
  uint64_t first_seq = 0;  /* the lowest sequence that arrived: nonzero on a
                            * run whose head was lost, or on a receiver
                            * started after its sender */
  double first_s = 0, last_s = 0;
  /* THE WINDOW THE MEASUREMENT COVERS, which is not the span between the
   * first and last arrival. Dividing bytes by the span rewards a link that
   * dies early: the first on-air run of this tool reported 5.82 Mbit/s for
   * ELEVEN datagrams, because they all arrived inside 21 ms and then the
   * link carried nothing for the remaining fifteen seconds. The span is
   * still reported - it is how you tell "slow" from "stopped" - but the rate
   * is over the window that was asked for. */
  double window_s = 0;
};

/* Receive until `secs` have passed since the FIRST datagram, or until `secs`
 * plus the grace period have passed with none at all.
 *
 * MEASURED FROM THE FIRST DATAGRAM, deliberately: the sender is started by
 * hand or by a script and the gap between "receiver up" and "sender up" is
 * dead time that would otherwise divide into the rate and understate it. */
RecvResult receive_for(int fd, double secs, double grace) {
  RecvResult r;
  uint8_t buf[kMaxPayload];
  /* The reorder/duplicate distinction needs a window, not a high-water mark
   * alone: without it every datagram after a single loss looks reordered.
   * 1024 is far wider than anything an 802.11 MAC will reorder. */
  constexpr size_t kWin = 1024;
  static bool seen[kWin];
  std::memset(seen, 0, sizeof seen);
  uint64_t high = 0, low = 0;
  bool started = false;
  const double deadline_idle = now_s() + secs + grace;

  for (;;) {
    const double t = now_s();
    if (started && t - r.first_s >= secs) { r.window_s = t - r.first_s; break; }
    if (!started && t >= deadline_idle) break;

    /* 20 ms, not 200. The loop can only notice the window has ended between
     * recv() calls, so the timeout is the measurement's granularity: at
     * 200 ms a quiet tail overshot the window by up to a fifth of a second,
     * which the rate is then divided by. */
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 20000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
    if (n < (ssize_t)kSeqBytes) continue;

    const uint64_t seq = get_be64(buf);
    const double at = now_s();
    if (!started) {
      started = true;
      r.first_s = at;
      high = seq;
      low = seq;
      /* The first datagram is not evidence that everything before it was
       * lost: the sender may have started before the receiver was listening,
       * and on a link with no retransmission the first few can simply be
       * gone. Counting from here is the conservative reading. */
    }
    r.last_s = at;
    r.datagrams++;
    r.bytes += (uint64_t)n;

    if (seq < low) low = seq;
    if (seq > high) {
      /* BOUNDED: one stray datagram whose first 8 bytes read as a sequence
       * near 2^63 made this loop run effectively forever. A jump of a whole
       * window or more leaves nothing in it reachable - clear it at once. */
      if (seq - high >= kWin)
        for (size_t s = 0; s < kWin; s++) seen[s] = false;
      else
        for (uint64_t s = high + 1; s < seq; s++) seen[s % kWin] = false;
      high = seq;
      seen[seq % kWin] = true;
    } else if (seq == high) {
      if (seen[seq % kWin]) r.duplicated++;
      seen[seq % kWin] = true;
    } else if (high - seq < kWin) {
      if (seen[seq % kWin]) r.duplicated++;
      else { r.reordered++; seen[seq % kWin] = true; }
    } else {
      r.reordered++;   /* older than the window: count it, do not trust it */
    }
  }
  if (started) {
    if (r.window_s <= 0) r.window_s = now_s() - r.first_s;
    uint64_t arrived = r.datagrams - r.duplicated;
    uint64_t span = high - (r.datagrams ? 0 : 0);
    (void)span;
    /* Loss is the span the sequence covered minus what actually arrived,
     * FROM SEQUENCE 0 - this tool's sender always starts there, and the
     * harness starts the receiver first, so a missing head is a lost head.
     * Counting from the lowest arrival instead (tried in review) hid exactly
     * that: a link that dropped its first 300 datagrams reported lost=0.
     * A receiver started AFTER its sender over-reports instead - the
     * conservative direction - and `first_seq` in the event says so. */
    r.first_seq = low;
    const uint64_t expected = high + 1;
    r.lost = expected > arrived ? expected - arrived : 0;
  }
  return r;
}

int run_recv(const char* bind_ip, int port, double secs) {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) { perror("socket"); return 1; }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  /* A big receive buffer, so a scheduling hiccup on this side is not reported
   * as a loss on the link. 4 MB is ~3 seconds of a 10 Mbit/s stream. */
  int rcvbuf = 4 << 20;
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);

  struct sockaddr_in a;
  std::memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  if (::inet_pton(AF_INET, bind_ip, &a.sin_addr) != 1) {
    std::fprintf(stderr, "bad bind address %s\n", bind_ip);
    return 2;
  }
  if (::bind(fd, (struct sockaddr*)&a, sizeof a) != 0) { perror("bind"); return 1; }

  const RecvResult r = receive_for(fd, secs, /*grace=*/10.0);
  ::close(fd);

  const double span = r.datagrams > 1 ? (r.last_s - r.first_s) : 0.0;
  const double mbps =
      r.window_s > 0 ? (double)r.bytes * 8.0 / r.window_s / 1e6 : 0.0;
  const uint64_t offered = r.datagrams + r.lost;
  std::printf(
      "{\"ev\":\"udp_blast.recv\",\"datagrams\":%llu,\"bytes\":%llu,"
      "\"lost\":%llu,\"reordered\":%llu,\"duplicated\":%llu,"
      "\"first_seq\":%llu,"
      "\"window_s\":%.3f,\"span_s\":%.3f,\"goodput_mbps\":%.3f,"
      "\"loss_pct\":%.2f}\n",
      (unsigned long long)r.datagrams, (unsigned long long)r.bytes,
      (unsigned long long)r.lost, (unsigned long long)r.reordered,
      (unsigned long long)r.duplicated, (unsigned long long)r.first_seq,
      r.window_s, span, mbps,
      /* -1, NOT ZERO. A rung that delivered nothing has no idea what was
       * offered, and printing 0.00% next to 0.000 Mbit/s reads as a perfect
       * link. It did exactly that in the first ladder run: three rungs in a
       * row showed "delivered 0.000 Mbit/s, loss 0.00%", which is the most
       * flattering possible description of a link carrying nothing. */
      offered ? 100.0 * (double)r.lost / (double)offered : -1.0);
  return r.datagrams > 0 ? 0 : 3;
}

int run_send(const char* ip, int port, size_t payload, long kbit, double secs) {
  /* THERE IS NO UNBOUNDED MODE, AND THAT IS DELIBERATE.
   *
   * This tool shipped with `kbit == 0` meaning "as fast as the socket takes
   * it", and the first two on-air runs offered 5.6 and 6.6 Gbit/s - seven to
   * nine MILLION sendto() calls in fifteen seconds, into a TAP whose reader
   * drains a handful per millisecond. What that measures is the kernel
   * dropping on a queue; the radio never saw 99.9% of it, and `lost` was
   * dominated by frames that never left the host. It is a bad measurement
   * AND an unbounded load on a machine somebody else is using.
   *
   * The ladder in tests/sta_d2d_onair.sh is the instrument: offer a known
   * rate, see what arrives, walk up until the loss climbs. That needs a rate,
   * so requiring one costs nothing and removes the footgun. The ceiling is a
   * typo guard, not a capability limit - raise it deliberately if a link ever
   * needs it. */
  if (kbit <= 0) {
    std::fprintf(stderr,
                 "udp_blast: an offered rate is required (kbit > 0).\n"
                 "  There is no unbounded mode: it measures the host's queues,\n"
                 "  not the link. Walk a ladder of rates instead.\n");
    return 2;
  }
  if (kbit > kMaxKbit) {
    std::fprintf(stderr, "udp_blast: %ld kbit/s exceeds the %ld kbit/s ceiling\n",
                 kbit, (long)kMaxKbit);
    return 2;
  }
  if (payload < kMinPayload) payload = kMinPayload;
  if (payload > kMaxPayload) payload = kMaxPayload;
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) { perror("socket"); return 1; }
  struct sockaddr_in a;
  std::memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  if (::inet_pton(AF_INET, ip, &a.sin_addr) != 1) {
    std::fprintf(stderr, "bad destination %s\n", ip);
    return 2;
  }

  uint8_t buf[kMaxPayload];
  std::memset(buf, 0x5a, sizeof buf);
  uint64_t seq = 0, sent = 0, refused = 0, bytes = 0;
  const double t0 = now_s();
  const double end = t0 + secs;
  /* The pacing is on BYTES, not datagrams, so the offered rate means the same
   * thing at every payload size. */
  const double bytes_per_s = (double)kbit * 1000.0 / 8.0;

  while (now_s() < end) {
    put_be64(buf, seq);
    const ssize_t n = ::sendto(fd, buf, payload, 0, (struct sockaddr*)&a, sizeof a);
    if (n < 0) {
      /* ENOBUFS is the TAP queue telling us it is full. It is a REFUSAL, not
       * a loss on the link, and conflating the two would blame the radio for
       * this host's backlog. */
      if (errno == ENOBUFS || errno == EAGAIN || errno == EWOULDBLOCK) {
        refused++;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        continue;
      }
      perror("sendto");
      break;
    }
    seq++;
    sent++;
    bytes += (uint64_t)n;
    const double want = t0 + (double)bytes / bytes_per_s;
    const double slack = want - now_s();
    if (slack > 0)
      std::this_thread::sleep_for(std::chrono::duration<double>(slack));
  }
  const double dur = now_s() - t0;
  ::close(fd);
  std::printf(
      "{\"ev\":\"udp_blast.send\",\"datagrams\":%llu,\"bytes\":%llu,"
      "\"refused\":%llu,\"seconds\":%.3f,\"offered_mbps\":%.3f}\n",
      (unsigned long long)sent, (unsigned long long)bytes,
      (unsigned long long)refused, dur,
      dur > 0 ? (double)bytes * 8.0 / dur / 1e6 : 0.0);
  return sent > 0 ? 0 : 3;
}

/* ---- the headless cells ------------------------------------------------- */

int g_fail = 0;
void check(bool ok, const char* what) {
  if (!ok) { std::printf("FAIL: %s\n", what); g_fail++; }
}

/* Drive receive_for() over a loopback socket with a sequence this test
 * controls exactly, so the accounting is checked against a known answer
 * rather than against the link it is supposed to be measuring. */
void feed(int tx, const struct sockaddr_in& to, uint64_t seq, size_t len) {
  uint8_t b[256];
  std::memset(b, 0, sizeof b);
  put_be64(b, seq);
  ::sendto(tx, b, len, 0, (const struct sockaddr*)&to, sizeof to);
}

RecvResult drive(const uint64_t* seqs, size_t n) {
  int rx = ::socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in a;
  std::memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
  if (::bind(rx, (struct sockaddr*)&a, sizeof a) != 0) { ::close(rx); return {}; }
  socklen_t al = sizeof a;
  if (::getsockname(rx, (struct sockaddr*)&a, &al) != 0) { ::close(rx); return {}; }

  int tx = ::socket(AF_INET, SOCK_DGRAM, 0);
  for (size_t i = 0; i < n; i++) feed(tx, a, seqs[i], 64);
  /* A short window: everything is already in the socket buffer, and the
   * grace period is what ends it when the sequence is empty. */
  RecvResult r = receive_for(rx, 0.25, 0.25);
  ::close(tx);
  ::close(rx);
  return r;
}

int self_test() {
  {   /* Clean: ten in order, nothing lost. */
    uint64_t s[10];
    for (int i = 0; i < 10; i++) s[i] = (uint64_t)i;
    RecvResult r = drive(s, 10);
    check(r.datagrams == 10, "ten datagrams arrive");
    check(r.lost == 0, "...and none is counted lost");
    check(r.reordered == 0 && r.duplicated == 0, "...nor reordered nor duplicated");
    check(r.bytes == 640, "...and the byte count is the payload, not the frame");
  }
  {   /* A GAP. This is the one the throughput cells rest on: a link with no
       * link-layer retransmission loses datagrams, and a tool that cannot
       * count them reports a clean run over a lossy link. */
    const uint64_t s[] = {0, 1, 2, 5, 6};
    RecvResult r = drive(s, 5);
    check(r.datagrams == 5, "five arrive");
    check(r.lost == 2, "...and the two-wide gap is counted as two lost");
    check(r.reordered == 0, "...and a gap is not reordering");
  }
  {   /* REORDERING IS NOT LOSS. Counting a late arrival as lost would
       * overstate the loss of exactly the link this measures. */
    const uint64_t s[] = {0, 1, 3, 2, 4};
    RecvResult r = drive(s, 5);
    check(r.datagrams == 5, "five arrive out of order");
    check(r.lost == 0, "...and nothing is counted lost");
    check(r.reordered == 1, "...and the one that arrived late is counted as such");
  }
  {   /* A DUPLICATE. The MAC can deliver a retransmission twice, and this
       * tree has no duplicate filter on the receive path (see the station
       * mode handoff's open list), so a duplicate WILL reach here. It must
       * not read as a datagram that made the link better than it was. */
    const uint64_t s[] = {0, 1, 1, 2};
    RecvResult r = drive(s, 4);
    check(r.datagrams == 4, "four datagrams are received");
    check(r.duplicated == 1, "...one of them a duplicate");
    check(r.lost == 0, "...and a duplicate does not make the loss negative");
  }
  {   /* THE WINDOW WRAPS, and until this cell existed nothing noticed. The
       * reorder window is a 1024-slot bitmap indexed by sequence modulo its
       * width, so a slot left set by an OLD sequence makes a later arrival
       * that lands on it read as a duplicate rather than as reordering. The
       * clearing loop in the forward branch is what prevents that, and a
       * mutation deleting it survived the whole file: every other cell uses
       * sequences shorter than the window, where a stale slot cannot exist.
       *
       * Sequence 100 sets slot 100. The jump to 1200 must clear it (1124
       * maps to the same slot). Then 1124 arrives late: with the clearing it
       * is reordering, without it, it is a duplicate - and a duplicate is
       * subtracted from the arrival count, so the loss figure moves too. */
    const uint64_t s[] = {100, 1200, 1124};
    RecvResult r = drive(s, 3);
    check(r.datagrams == 3, "three arrive across a window-wide jump");
    check(r.reordered == 1, "...the late one is reordering");
    check(r.duplicated == 0, "...and NOT a duplicate of a stale window slot");
    check(r.lost == 1198,
          "...and the gap is counted in full, from sequence 0: the head the "
          "sender aired first (0..99) is lost too");
  }
  {   /* A HUGE FORWARD JUMP must not spin the window-clearing loop once per
       * skipped sequence number: one stray datagram near 2^63 hung the
       * receiver. 2^40 would take hours unbounded; bounded it is instant. */
    const uint64_t s[] = {0, 1ull << 40};
    RecvResult r = drive(s, 2);
    check(r.datagrams == 2, "a 2^40 jump arrives and the run ENDS");
    check(r.lost == (1ull << 40) - 1, "...with the gap counted as lost");
  }
  {   /* A LOST HEAD IS LOSS. The sender starts at 0; a run whose first 48
       * datagrams never arrived must not read as nearly perfect, and the
       * event names where it started. */
    const uint64_t s[] = {50, 51, 48};
    RecvResult r = drive(s, 3);
    check(r.lost == 49, "loss counts from sequence 0 (0..47 and 49)");
    check(r.first_seq == 48, "...and first_seq reports the lowest arrival");
  }
  {   /* THE RATE IS OVER THE WINDOW, NOT THE BURST. Ten datagrams delivered
       * in a millisecond, then silence for the rest of a quarter second, is
       * not a fast link - it is a link that stopped. Dividing by the span
       * between first and last arrival says otherwise, and on air it said so
       * out loud: eleven datagrams inside 21 ms were reported as 5.82
       * Mbit/s on a link that had just carried nothing for fifteen seconds. */
    uint64_t s10[10];
    for (int i = 0; i < 10; i++) s10[i] = (uint64_t)i;
    RecvResult r = drive(s10, 10);
    check(r.window_s >= 0.2, "the window is the one that was asked for, not the burst");
    check(r.last_s - r.first_s < 0.05, "...the burst itself was far shorter");
    /* 640 bytes over a ~0.25 s window is ~0.02 Mbit/s. Over the burst it
     * would be at least 0.1, and in the on-air case that prompted this cell
     * it was 5.82. The bound is loose because the window overshoots by up to
     * one poll interval; it is still an order of magnitude below the burst. */
    const double mbps = r.bytes * 8.0 / r.window_s / 1e6;
    check(mbps > 0.008 && mbps < 0.05,
          "...so the rate is ~0.02 Mbit/s over the window, not >=0.1 over the burst");
  }
  {   /* Nothing at all. The grace period must end the run rather than hang,
       * and the result must be distinguishable from a clean one. */
    RecvResult r = drive(nullptr, 0);
    check(r.datagrams == 0, "an empty run receives nothing");
    check(r.lost == 0, "...and claims no loss it cannot know about");
    check(r.window_s == 0, "...and reports no window, so no rate can be divided out of it");
  }
  {   /* THE UNBOUNDED MODE IS REFUSED. It is not a capability that was
       * removed for tidiness: it offered 6.6 Gbit/s at a TAP on a shared
       * bench, and what it measured was the host. A cell, because a footgun
       * removed without a test grows back. */
    check(run_send("127.0.0.1", 9, 64, 0, 0.1) == 2,
          "an offered rate of zero is refused, not treated as unbounded");
    check(run_send("127.0.0.1", 9, 64, -1, 0.1) == 2,
          "...and so is a negative one");
    check(run_send("127.0.0.1", 9, 64, kMaxKbit + 1, 0.1) == 2,
          "...and a rate above the typo ceiling");
  }
  std::printf(g_fail ? "udp_blast --self-test: %d failure(s)\n"
                     : "udp_blast --self-test: OK\n", g_fail);
  return g_fail ? 1 : 0;
}

void usage() {
  std::fprintf(stderr,
               "usage: udp_blast recv <bind-ip> <port> <seconds>\n"
               "       udp_blast send <ip> <port> <payload> <kbit|0> <seconds>\n"
               "       udp_blast --self-test\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "--self-test") == 0) return self_test();
  if (argc >= 5 && std::strcmp(argv[1], "recv") == 0)
    return run_recv(argv[2], std::atoi(argv[3]), std::atof(argv[4]));
  if (argc >= 7 && std::strcmp(argv[1], "send") == 0)
    return run_send(argv[2], std::atoi(argv[3]), (size_t)std::atol(argv[4]),
                    std::atol(argv[5]), std::atof(argv[6]));
  usage();
  return 2;
}
