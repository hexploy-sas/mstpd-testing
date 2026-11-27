/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ftest-sys harness: real mstpd and mstpctl against the kernel bridge.
 *
 * The test process enters its own user (if not root), mount, network and pid
 * namespaces, so it needs no privilege and leaves the host network alone.
 * Each DUT is a network namespace with a bridge br0 in user STP mode
 * (IFLA_BR_STP_MODE, kernel >= 7.1) and its own mstpd. DUTs are linked by
 * veth pairs.
 *
 * Binaries: $MSTPD and $MSTPCTL (default ./mstpd and ./mstpctl).
 * Logs: one mstpd log per DUT in $FTEST_LOGDIR (default ./ftest-logs).
 */

#ifndef TESTS_SYS_LIB_SYS_H
#define TESTS_SYS_LIB_SYS_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <sys/types.h>

#include <cmocka.h>

#include "libnetlink.h"
#include "stp_params.h"

/* mstpd timers used by the harness (Hello Time is fixed at 2 s) */
#define SYS_MAX_AGE MAX_AGE_MIN
#define SYS_FWD_DELAY FWD_DELAY_MIN

/* result of a bounded request still blocked at its deadline */
#define SYS_BLOCKED (-2)

/* polling period of sys_wait() */
#define SYS_POLL_MS 10

struct dut {
    char name[16];
    int netns;                  /* network namespace fd */
    struct rtnl_handle rth;     /* rtnetlink socket in the DUT namespace */
    pid_t mstpd;
    char log[256];
};

/* Enter the test namespaces. Call first in main(), before cmocka. Returns 0
 * when ready; otherwise sys_skip_reason() tells why. */
int sys_init(void);
const char *sys_skip_reason(void);
/* skip() the current test if the environment is not ready */
void sys_require(void);

/* new DUT: namespace and bridge br0 with MAC 02:00:00:00:00:<mac_last>, up */
int dut_create(struct dut *d, const char *name, unsigned char mac_last);
/* veth pair a:aport <-> b:bport, both ends in br0 and up */
int dut_link(struct dut *a, const char *aport, struct dut *b, const char *bport);
/* start mstpd, add br0 and set the harness timers */
int dut_start(struct dut *d);
/* stop mstpd and release the namespace (its veth ends go away) */
void dut_destroy(struct dut *d);

/* run mstpctl in the DUT namespace with a NULL-terminated argument list;
 * stdout goes to out (may be NULL); returns the exit code */
int dut_ctl(struct dut *d, char *out, size_t outlen, ...);
/* administrative state of an interface in the DUT */
int dut_set_link(struct dut *d, const char *ifname, bool up);

/* kernel STP state of a br0 port (BR_STATE_*), -1 on error */
int dut_port_state(struct dut *d, const char *port);
/* port role from mstpctl ("Root", "Designated", "Alternate", ...) */
int dut_port_role(struct dut *d, const char *port, char *role, size_t len);
/* bridge ID and designated root from mstpctl, as printed */
int dut_bridge_ids(struct dut *d, char *bridge_id, char *root_id, size_t len);

/* milliseconds on a monotonic clock */
long sys_now_ms(void);

/* poll pred(arg) every SYS_POLL_MS; returns the elapsed time in ms, or -1 on
 * timeout */
long sys_wait(bool (*pred)(void *), void *arg, long timeout_ms);

#endif
