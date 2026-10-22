#pragma once
#include <switch.h>

#include "config.h"

// Atmosphère mitm of usb:hs that forwards every request unchanged and logs it (approach C-0).
//
// Installs on each service in cfg->services, serves sm's ShouldMitm queries (yes only for the
// programs in cfg), and proxies the intercepted sessions and the IClientIfSession/IClientEpSession
// objects they open. Main thread only. Returns the number of services installed.
int mitm_start(const lm_config *cfg);
// Uninstalls from sm. Safe to call from the exception handler (best effort, no locks).
void mitm_uninstall_all(void);
// One line with counters (main thread, every minute).
void mitm_status(char *buf, size_t size);
