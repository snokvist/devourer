/* Headless guard for examples/common/env_config.cpp - the demos' env-var to
 * DeviceConfig mapping.
 *
 * One cell today, for a defect that shipped: `DEVOURER_TX_RETRY_LIMIT`'s
 * parse was an unbraced `if` over TWO statements, so `retry_limit_set` was
 * raised on every call whether or not the variable existed. Realtek backends
 * ignore the flag; the MT7612U honours it, and wrote its Realtek-convention
 * default of 0 into MT_TX_RETRY_CFG - hardware retransmission off for every
 * MT7612U demo session that did not happen to export the variable. The on-air
 * scripts all export it, which is why nothing on a bench saw it.
 */
#include <cstdio>
#include <cstdlib>

#include "env_config.h"

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    g_fail++;
  }
}

void set_env(const char* name, const char* value) {
#ifdef _WIN32
  _putenv_s(name, value ? value : "");   /* "" removes it */
#else
  if (value)
    setenv(name, value, 1);
  else
    unsetenv(name);
#endif
}

void test_retry_limit_is_set_only_when_given() {
  set_env("DEVOURER_TX_RETRY_LIMIT", nullptr);
  devourer::DeviceConfig cfg = devourer_config_from_env();
  check(!cfg.tx.retry_limit_set,
        "DEVOURER_TX_RETRY_LIMIT unset leaves retry_limit_set FALSE");

  set_env("DEVOURER_TX_RETRY_LIMIT", "5");
  cfg = devourer_config_from_env();
  check(cfg.tx.retry_limit_set && cfg.tx.retry_limit == 5,
        "DEVOURER_TX_RETRY_LIMIT=5 sets both the flag and the value");
  set_env("DEVOURER_TX_RETRY_LIMIT", nullptr);
}

}  // namespace

int main() {
  test_retry_limit_is_set_only_when_given();
  if (g_fail) {
    std::printf("env_config_selftest: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("env_config_selftest: OK\n");
  return 0;
}
