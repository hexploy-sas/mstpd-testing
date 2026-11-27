/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ftest-sys, host mode: STP enabled through the kernel helper
 * /sbin/bridge-stp (the build's bridge-stp script). The kernel calls the
 * helper only for bridges of the init network namespace, so these tests
 * run as root in a disposable VM (make check-sys-vm) and skip elsewhere.
 *   dut0 (init namespace) br0 e1 <-> neighbour A
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <linux/if_bridge.h>

#include "sys.h"

#define BLOCK_MS 10000
#define STATE_MS 5000
#define FORWARD_MS 15000

/* kernel stp_state values */
#define STP_USER 2

struct topo {
    struct dut d;
    struct vnb a;
    bool hung;
};

static struct topo topo;

static int setup(void **state)
{
    memset(&topo, 0, sizeof(topo));
    topo.d.netns = topo.a.fd = -1;
    if (!sys_skip_reason())
        sys_kill_mstpd();
    *state = &topo;
    return 0;
}

static int teardown(void **state)
{
    struct topo *t = &topo;

    if (sys_skip_reason() || t->d.netns < 0)
        return 0;
    /* a blocked kernel request would block the cleanup too */
    if (!t->hung) {
        dut_set_stp(&t->d, false, BLOCK_MS);
        vnb_destroy(&t->a);
        dut_del_link(&t->d, "br0");
    }
    sys_kill_mstpd();
    dut_destroy(&t->d);
    return 0;
}

static bool stp_user(void *arg)
{
    return dut_stp_state(arg) == STP_USER;
}

/* e1 without BPDU from the neighbour: designated, auto edge, forwarding */
static bool e1_forwarding(void *arg)
{
    struct dut *d = arg;
    char role[32];

    return dut_port_state(d, "e1") == BR_STATE_FORWARDING
           && !dut_port_role(d, "e1", role, sizeof(role))
           && !strcmp(role, "Designated");
}

static void assert_managed(struct topo *t)
{
    assert_true(sys_wait(stp_user, &t->d, STATE_MS) >= 0);
    assert_true(sys_mstpd_running());
    assert_true(sys_wait(e1_forwarding, &t->d, FORWARD_MS) >= 0);
}

static void add_port(struct topo *t)
{
    assert_int_equal(vnb_link(&t->a, "nbA", 0xa, &t->d, "e1"), 0);
    assert_int_equal(dut_ports_up(&t->d), 0);
}

static void set_stp_on(struct topo *t)
{
    int r = dut_set_stp(&t->d, true, BLOCK_MS);

    t->hung = r == SYS_BLOCKED;
    assert_false(t->hung);
    assert_int_equal(r, 0);
}

/* mstpd already running: stp_state 1 calls the helper, which adds the
 * bridge to mstpd; the kernel switches to user STP */
static void helper_attaches_running_mstpd(void **state)
{
    struct topo *t = *state;

    sys_require();
    assert_int_equal(dut_create_host(&t->d, "dut0", 0x10, false, BLOCK_MS),
                     0);
    add_port(t);
    assert_int_equal(dut_start_mstpd(&t->d), 0);
    set_stp_on(t);
    assert_managed(t);
}

/* mstpd not running: the helper starts it, then adds the bridge */
static void helper_starts_mstpd(void **state)
{
    struct topo *t = *state;

    sys_require();
    assert_false(sys_mstpd_running());
    assert_int_equal(dut_create_host(&t->d, "dut0", 0x10, false, BLOCK_MS),
                     0);
    add_port(t);
    set_stp_on(t);
    assert_managed(t);
}

/* Bridge created with stp_state 1 in one request while mstpd is not running
 * (#32): the helper starts mstpd during the bridge creation; no hang */
static void helper_bridge_created_with_stp(void **state)
{
    struct topo *t = *state;
    int r;

    sys_require();
    assert_false(sys_mstpd_running());
    r = dut_create_host(&t->d, "dut0", 0x10, true, BLOCK_MS);
    t->hung = r == SYS_BLOCKED;
    assert_false(t->hung);
    assert_int_equal(r, 0);
    add_port(t);
    assert_managed(t);
}

int main(int argc, char *argv[])
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(helper_attaches_running_mstpd, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(helper_starts_mstpd, setup, teardown),
        cmocka_unit_test_setup_teardown(helper_bridge_created_with_stp, setup,
                                        teardown),
    };

    if (!sys_init_host() && sys_install_helper())
        fprintf(stderr, "cannot install the helper\n");
    return cmocka_run_group_tests(tests, NULL, NULL);
}
