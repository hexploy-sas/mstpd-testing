/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ftest-sys: bridge ports changing around mstpd start and addbridge.
 * One DUT; ports are veth pairs to virtual neighbours or dummy interfaces.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <linux/if_bridge.h>

#include "sys.h"

#define FORWARD_MS 10000
#define SETTLE_MS 3000
#define CARRIER_LOSS_MS 2000
#define ITERATIONS 100
#define KEPT_PORTS 3
#define MAX_PORTS 16

struct topo {
    struct dut d;
    struct vnb a, b;
};

static struct topo topo;

static int setup(void **state)
{
    memset(&topo, 0, sizeof(topo));
    topo.d.netns = topo.a.fd = topo.b.fd = -1;
    *state = &topo;
    return 0;
}

static int teardown(void **state)
{
    dut_destroy(&topo.d);
    vnb_destroy(&topo.a);
    vnb_destroy(&topo.b);
    return 0;
}

struct port_wait {
    struct dut *d;
    const char *port;
};

/* designated, forwarding in the kernel (auto edge, no BPDU) */
static bool forwarding(void *arg)
{
    struct port_wait *w = arg;
    char role[32];

    return dut_port_state(w->d, w->port) == BR_STATE_FORWARDING
           && !dut_port_role(w->d, w->port, role, sizeof(role))
           && !strcmp(role, "Designated");
}

/* Ports down (d3f5387, e353c42): e2 has no carrier when mstpd starts, gets
 * its STP states once the carrier is up, then loses it again. mstpd never
 * sets a kernel state the kernel refuses on a port without carrier: no
 * error is logged */
static void ports_down_at_start(void **state)
{
    struct topo *t = *state;
    struct port_wait e1 = { &t->d, "e1" }, e2 = { &t->d, "e2" };

    sys_require();
    assert_int_equal(dut_create(&t->d, "dut0", 0x10), 0);
    assert_int_equal(vnb_link(&t->a, "nbA", 0xa, &t->d, "e1"), 0);
    assert_int_equal(vnb_link(&t->b, "nbB", 0xb, &t->d, "e2"), 0);
    assert_int_equal(vnb_set_link(&t->b, false), 0);
    assert_int_equal(dut_start(&t->d), 0);

    assert_true(sys_wait(forwarding, &e1, FORWARD_MS) >= 0);
    assert_int_not_equal(dut_port_state(&t->d, "e2"), BR_STATE_FORWARDING);
    assert_int_equal(dut_log_count(&t->d, "error"), 0);

    assert_int_equal(vnb_set_link(&t->b, true), 0);
    assert_true(sys_wait(forwarding, &e2, FORWARD_MS) >= 0);
    assert_int_equal(dut_log_count(&t->d, "error"), 0);

    /* carrier lost on a forwarding port */
    assert_int_equal(vnb_set_link(&t->b, false), 0);
    usleep(CARRIER_LOSS_MS * 1000);
    assert_int_not_equal(dut_port_state(&t->d, "e2"), BR_STATE_FORWARDING);
    assert_int_equal(dut_log_count(&t->d, "error"), 0);
}

/* every kernel port of br0 is known to mstpd and enabled */
static bool ports_managed(void *arg)
{
    struct dut *d = arg;
    char names[MAX_PORTS][16], val[16];
    int n, i;

    n = dut_br_ports(d, names, MAX_PORTS);
    if (n < 0)
        return false;
    for (i = 0; i < n; i++)
        if (dut_port_param(d, names[i], "enabled", val, sizeof(val))
            || strcmp(val, "yes"))
            return false;
    return true;
}

/* Ports added and removed while mstpctl delbridge/addbridge run (3ccdf6b):
 * after each round, every kernel port of br0 is managed and enabled; mstpd
 * survives */
static void ports_during_addbridge(void **state)
{
    struct topo *t = *state;
    char name[16];
    int i, status;
    pid_t pid;

    sys_require();
    assert_int_equal(dut_create(&t->d, "dut0", 0x10), 0);
    assert_int_equal(dut_start(&t->d), 0);

    for (i = 0; i < ITERATIONS; i++) {
        assert_int_equal(dut_ctl(&t->d, NULL, 0, "delbridge", "br0",
                                 (char *)NULL), 0);
        pid = fork();
        assert_true(pid >= 0);
        if (pid == 0) {
            if (dut_reopen(&t->d))
                _exit(1);
            snprintf(name, sizeof(name), "dA%d", i);
            if (dut_add_dummy(&t->d, name))
                _exit(1);
            if (i >= KEPT_PORTS) {
                snprintf(name, sizeof(name), "dA%d", i - KEPT_PORTS);
                if (dut_del_link(&t->d, name))
                    _exit(1);
            }
            _exit(0);
        }
        assert_int_equal(dut_ctl(&t->d, NULL, 0, "addbridge", "br0",
                                 (char *)NULL), 0);
        assert_int_equal(waitpid(pid, &status, 0), pid);
        assert_true(WIFEXITED(status) && !WEXITSTATUS(status));
        if (sys_wait(ports_managed, &t->d, SETTLE_MS) < 0)
            fail_msg("round %d: a kernel port of br0 is not managed", i);
    }
    /* errors are expected in the log: dummy interfaces have no speed for
     * ethtool, and ports disappear while mstpd handles them */
    assert_true(dut_mstpd_alive(&t->d));
}

int main(int argc, char *argv[])
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(ports_down_at_start, setup, teardown),
        cmocka_unit_test_setup_teardown(ports_during_addbridge, setup,
                                        teardown),
    };

    sys_init();
    return cmocka_run_group_tests(tests, NULL, NULL);
}
