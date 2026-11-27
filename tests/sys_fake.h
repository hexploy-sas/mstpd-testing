/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Fake system layer for tests that link the real bridge_track.c: interface
 * table for netif_utils, recorders for packet_send(), rtnl_talk() and sysfs
 * writes (linked with -Wl,--wrap=rtnl_talk,--wrap=open), logging.
 */

#ifndef TESTS_SYS_FAKE_H
#define TESTS_SYS_FAKE_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>

#include <cmocka.h>

#include <linux/types.h>

/* forget all interfaces and recorded calls */
void sys_fake_reset(void);
/* declare a bridge interface */
void sys_fake_add_bridge(int ifindex, const char *name, __u64 mac);
/* declare a bridge port interface with its bridge port number */
void sys_fake_add_port(int ifindex, const char *name, __u64 mac, int portno);
/* set interface flags (IFF_UP, IFF_RUNNING) returned by get_flags() */
void sys_fake_set_flags(int ifindex, unsigned int flags);

/* number of kernel STP state changes requested for that interface (RTM_SETLINK
 * with IFLA_PROTINFO), and the last requested state (-1 if none) */
int sys_fake_num_set_state(int ifindex, int *last_state);
/* number of frames sent on that interface */
int sys_fake_num_tx(int ifindex);
/* number of if_shutdown() calls for that interface */
int sys_fake_num_shutdown(int ifindex);
/* number of forwarding database flushes requested for that port (sysfs) */
int sys_fake_num_flush(int ifindex);
/* number of ageing time changes requested for that bridge (sysfs) */
int sys_fake_num_set_ageing(int ifindex);
/* number of messages logged at error level */
int sys_fake_num_errors(void);

#endif
