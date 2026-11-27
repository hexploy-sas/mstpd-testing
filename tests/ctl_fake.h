/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Fake mstpctl client side for tests that include ctl_main.c: records the
 * last request instead of sending it to the daemon, and resolves interface
 * names (linked with -Wl,--wrap=if_nametoindex).
 */

#ifndef TESTS_CTL_FAKE_H
#define TESTS_CTL_FAKE_H

#define CTL_FAKE_BR 10  /* br0 */
#define CTL_FAKE_P1 11  /* p1 */

/* forget the recorded request */
void ctl_fake_reset(void);
/* number of requests sent since the last reset */
int ctl_fake_num_msgs(void);
/* command code and input buffer of the last request */
int ctl_fake_last_cmd(void);
void *ctl_fake_last_in(void);

#endif
