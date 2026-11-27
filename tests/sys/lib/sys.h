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
 * Logs: one mstpd log per DUT in $FTEST_LOGDIR (default ./ftest-logs), at
 * mstpd log level $FTEST_MSTPD_LOGLEVEL (default 2).
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

#define DUT_MAX_PORTS 8

struct dut {
    char name[16];
    int netns;                  /* network namespace fd */
    struct rtnl_handle rth;     /* rtnetlink socket in the DUT namespace */
    pid_t mstpd;
    char log[256];
    char ports[DUT_MAX_PORTS][16]; /* br0 ports, brought up by dut_start() */
    int nports;
};

/* Enter the test namespaces. Call first in main(), before cmocka. Returns 0
 * when ready; otherwise sys_skip_reason() tells why. */
int sys_init(void);
const char *sys_skip_reason(void);
/* skip() the current test if the environment is not ready */
void sys_require(void);

/* new DUT: namespace and bridge br0 with MAC 02:00:00:00:00:<mac_last>, up */
int dut_create(struct dut *d, const char *name, unsigned char mac_last);
/* veth pair a:aport <-> b:bport, both ends in br0; each end stays down until
 * dut_start() of its DUT */
int dut_link(struct dut *a, const char *aport, struct dut *b, const char *bport);
/* start mstpd, add br0, set the harness timers, then bring the br0 ports
 * up (so that port timers start from the harness values) */
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
/* value of a showportdetail JSON field, e.g. "num-rx-tcn" */
int dut_port_param(struct dut *d, const char *port, const char *key,
                   char *val, size_t len);
/* port role from mstpctl ("Root", "Designated", "Alternate", ...) */
int dut_port_role(struct dut *d, const char *port, char *role, size_t len);
/* bridge ID and designated root from mstpctl, as printed */
int dut_bridge_ids(struct dut *d, char *bridge_id, char *root_id, size_t len);

/* mstpd still running */
bool dut_mstpd_alive(struct dut *d);
/* interface flags (IFF_UP, ...) in the DUT, -1 on error */
int dut_link_flags(struct dut *d, const char *ifname);
/* 1 if the FDB of br0 has mac on port, 0 if not, -1 on error */
int dut_fdb_has(struct dut *d, const char *port, const unsigned char *mac);

/* Virtual neighbour: the far end of a veth pair whose near end is a br0
 * port of a DUT; it sends crafted BPDUs and frames and captures the BPDUs of
 * the DUT through an AF_PACKET socket. Its MAC is 02:00:00:00:01:<mac_last>. */
struct vnb {
    char ifname[16];
    int fd;
    unsigned char mac[6];
    pid_t sender;               /* periodic sender process */
};

/* the DUT port stays down until dut_start() */
int vnb_link(struct vnb *n, const char *name, unsigned char mac_last,
             struct dut *d, const char *port);
/* one BPDU in an 802.3/LLC frame to the bridge group address; the 802.3
 * length is len + 3, the frame is padded to 60 bytes */
int vnb_send(struct vnb *n, const void *bpdu, size_t len);
/* any Ethernet frame (header included) */
int vnb_send_frame(struct vnb *n, const void *frame, size_t len);
/* send a BPDU now and then every period_ms until vnb_stop() */
int vnb_send_periodic(struct vnb *n, const void *bpdu, size_t len,
                      int period_ms);
void vnb_stop(struct vnb *n);
/* next BPDU sent by the DUT; returns its length, -1 on timeout (a zero
 * timeout returns a BPDU already queued, if any) */
int vnb_recv(struct vnb *n, void *bpdu, size_t len, long timeout_ms);
void vnb_destroy(struct vnb *n);
/* RST BPDU (bpdu_t) from designated port 1 (default priority) of bridge
 * (bridge_prio, 02:00:00:00:00:<bridge_mac_last>) with root
 * (root_prio, 02:00:00:00:00:<root_mac_last>) at cost; harness times */
void vnb_make_bpdu(void *buf, unsigned int flags, uint16_t root_prio,
                   unsigned char root_mac_last, uint32_t cost,
                   uint16_t bridge_prio, unsigned char bridge_mac_last);

/* milliseconds on a monotonic clock */
long sys_now_ms(void);

/* poll pred(arg) every SYS_POLL_MS; returns the elapsed time in ms, or -1 on
 * timeout */
long sys_wait(bool (*pred)(void *), void *arg, long timeout_ms);

#endif
