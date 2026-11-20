/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Spanning tree parameters for tests: ranges and defaults of 802.1D-2004
 * 17.13-17.14 (Table 17-1) as applied by mstpd, and helpers to write them.
 */

#ifndef TESTS_STP_PARAMS_H
#define TESTS_STP_PARAMS_H

#include <linux/ethtool.h>      /* SPEED_*, DUPLEX_* for set_port_state() */

/* Times, in seconds */
#define HELLO_TIME              2   /* fixed since 0.2.0 (e894644) */
#define MAX_AGE_MIN             6
#define MAX_AGE_MAX             40
#define MAX_AGE_DEFAULT         20
#define FWD_DELAY_MIN           4
#define FWD_DELAY_MAX           30
#define FWD_DELAY_DEFAULT       15
#define MIGRATE_TIME            3
/* received information is kept for 3 x Hello Time (17.21.23) */
#define RCVD_INFO_TIME          (3 * HELLO_TIME)
/* Max Age and Forward Delay must satisfy 2 * (Forward Delay - 1) >= Max Age */
#define TIMES_VALID(max_age, fwd_delay) (2 * ((fwd_delay) - 1) >= (max_age))

/* Transmit Hold Count */
#define TX_HOLD_MIN             1
#define TX_HOLD_MAX             10
#define TX_HOLD_DEFAULT         6

/* Priorities as configured with mstpctl: 0..15, in steps of 4096 for a
 * bridge and 16 for a port */
#define PRIO_MAX                15
#define BRIDGE_PRIO_DEFAULT     8   /* 32768 */
#define PORT_PRIO_DEFAULT       8   /* 128 */
#define BRIDGE_PRIO_STEP        4096
/* priority field of a bridge ID (host order) */
#define BRIDGE_PRIO_VALUE(prio) ((prio) * BRIDGE_PRIO_STEP)
/* priority bits returned by GET_PRIORITY_FROM_IDENTIFIER() */
#define ID_PRIO_BITS(prio)      ((prio) << 4)
/* port identifier: priority (0..15) and port number (host order) */
#define PORT_ID(prio, num)      (((prio) << 12) | (num))

#endif
