/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef TESTS_COMMON_H
#define TESTS_COMMON_H

/* cmocka 1.x does not include all required headers, so include them ourselves */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>

#include <cmocka.h>

#include "mstp.h"

/* 
 * assert_uint_equal() is only present in cmocka >= 2.0, so use
 * assert_int_equal() as a fallback. Same functionality, just worse output on
 * assertion.
 */
#ifndef assert_uint_equal
#define assert_uint_equal assert_int_equal
#endif

/* cmocka does not provide a todo() yet, so fall back to skip() */
#ifndef todo
#define todo skip
#endif

extern int log_level;

/* sets up **state to track bridges */
int prepare_test(void **state);
/* de-register and free all bridges */
int teardown_test(void **state);
/* alloc and register a bridge with ports */
int alloc_bridge_ports(void **state, bridge_t **br, const char *name, __u64 mac, port_t *(*p0)[], int num_ports);

/* connect two ports */
void link_ports(port_t *p1, port_t *p2);
/* diconnect two ports */
void unlink_ports(port_t *p1, port_t *p2);

/* sets link state of a port, and if connected also of the connected port */
void set_port_state(port_t *port, bool up, int speed, bool duplex);

/* inject a BPDU as received on that port */
void port_rx_bpdu(port_t *p, const void *data, size_t len);
/* return the BPDU last transmitted from that port */
int port_last_tx_bpdu(port_t *p, bpdu_t **data, size_t *len);
/* number of TCN BPDUs transmitted from that port */
int port_num_tx_tcn_bpdu(port_t *p);
/* number of BPDUs transmitted from that port */
int port_num_tx_bpdu(port_t *p);
/* number of MSTP_OUT_flush_all_fids() calls on that port, all trees */
int port_num_flush(port_t *p);
/* number of MSTP_OUT_shutdown_port() calls on that port */
int port_num_shutdown(port_t *p);
/* number of MSTP_OUT_set_ageing_time() calls on that port, and last value */
int port_num_set_ageing(port_t *p, unsigned int *last_ageing_time);
/* build a BPDU from a neighbour claiming to be root with (prio, mac), path
 * cost 0, port 1 with default priority, Message Age 0, default times */
void make_bpdu(bpdu_t *b, __u8 version, __u8 type, __u16 prio, __u64 mac,
               __u8 flags);
/* drop RST BPDUs received on that port, as an 802.1D-1998 bridge does */
void port_set_drop_rx_rst(port_t *p, bool drop);
/* clears the last transmitted BPDU */
void port_reset_last_tx_bpdu(port_t *p);
/* advances the time of all bridges by one tick (second) */
void test_one_second(void **state);
/* advances the time of all bridges by n ticks */
void test_ticks(void **state, int n);

#endif
