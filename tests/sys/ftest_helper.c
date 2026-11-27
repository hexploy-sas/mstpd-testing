/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ftest-sys, host mode: STP enabled through the kernel helper
 * /sbin/bridge-stp (the build's bridge-stp script). The kernel calls the
 * helper only for bridges of the init network namespace, so these tests
 * run as root in a disposable VM (make check-sys-vm) and skip elsewhere.
 *   dut0 (init namespace) br0 e1 <-> neighbour A, e2 <-> neighbour B
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <linux/if_bridge.h>

#include "sys.h"
#include "mstp.h"

#define BLOCK_MS 10000
#define STATE_MS 5000
#define FORWARD_MS 15000
/* a port without agreement forwards after Max Age + Hello Time */
#define NO_AGREEMENT_MS ((MAX_AGE_DEFAULT + HELLO_TIME + 5) * 1000)
#define PERIOD_MS 1000
/* path cost set by the configuration hook of bridge-stp restart */
#define HOOK_COST 4321
#define INF_PRIO BRIDGE_PRIO_VALUE(PRIO_MAX)
#define INF_MAC 0xf1

/* kernel stp_state values */
#define STP_USER 2

struct topo {
    struct dut d;
    struct vnb a, b;
    bool hung;
};

static struct topo topo;

static int setup(void **state)
{
    memset(&topo, 0, sizeof(topo));
    topo.d.netns = topo.a.fd = topo.b.fd = -1;
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
        vnb_destroy(&t->b);
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

struct port_wait {
    struct dut *d;
    const char *port;
};

static bool forwarding(void *arg)
{
    struct port_wait *w = arg;

    return dut_port_state(w->d, w->port) == BR_STATE_FORWARDING;
}

/* mstp_restart ("bridge-stp restart"): mstpd restarts, the bridge is added
 * again and the configuration command re-applies the config; e2, which gets
 * no agreement from B, does not forward before its timers expire */
static void helper_restart(void **state)
{
    struct topo *t = *state;
    const char *argv[] = { "/sbin/bridge-stp", "restart", NULL };
    struct port_wait e1 = { &t->d, "e1" }, e2 = { &t->d, "e2" };
    unsigned char bpdu[sizeof(bpdu_t)];
    char val[32];
    int pid;
    long ms;

    sys_require();
    assert_int_equal(dut_create_host(&t->d, "dut0", 0x10, false, BLOCK_MS),
                     0);
    assert_int_equal(vnb_link(&t->a, "nbA", 0xa, &t->d, "e1"), 0);
    assert_int_equal(vnb_link(&t->b, "nbB", 0xb, &t->d, "e2"), 0);
    vnb_make_bpdu(bpdu, BPDU_FLAGS_ROLE_SET(encodedRoleDesignated), INF_PRIO,
                  INF_MAC, 0, INF_PRIO, INF_MAC);
    assert_int_equal(vnb_send_periodic(&t->b, bpdu, RST_BPDU_SIZE, PERIOD_MS),
                     0);
    assert_int_equal(dut_ports_up(&t->d), 0);
    set_stp_on(t);
    assert_true(sys_wait(e1_forwarding, &t->d, FORWARD_MS) >= 0);
    assert_true(sys_wait(forwarding, &e2, NO_AGREEMENT_MS) >= 0);

    pid = sys_mstpd_pid();
    assert_int_equal(sys_run(argv, t->d.log, BLOCK_MS), 0);
    assert_true(sys_mstpd_pid() > 0);
    assert_int_not_equal(sys_mstpd_pid(), pid);
    assert_int_not_equal(dut_port_state(&t->d, "e2"), BR_STATE_FORWARDING);

    assert_int_equal(dut_port_param(&t->d, "e1", "admin-external-cost", val,
                                    sizeof(val)), 0);
    assert_int_equal(atoi(val), HOOK_COST);

    ms = sys_wait(forwarding, &e2, NO_AGREEMENT_MS);
    print_message("e2 forwarding %ld ms after the restart\n", ms);
    assert_true(ms >= (MAX_AGE_DEFAULT - HELLO_TIME) * 1000);
    assert_true(sys_wait(forwarding, &e1, FORWARD_MS) >= 0);
}

/* configuration command for bridge-stp restart: marks e1 with HOOK_COST */
static const char *write_hook(void)
{
    static char path[PATH_MAX];
    char mstpctl[PATH_MAX];
    FILE *f;

    snprintf(path, sizeof(path), "%s/config-hook.sh", sys_logdir());
    if (!realpath(getenv("MSTPCTL") ? getenv("MSTPCTL") : "./mstpctl",
                  mstpctl))
        return NULL;
    f = fopen(path, "w");
    if (!f)
        return NULL;
    fprintf(f, "#!/bin/sh\nexec '%s' setportpathcost \"$1\" e1 %d\n",
            mstpctl, HOOK_COST);
    if (fclose(f) || chmod(path, 0755))
        return NULL;
    return path;
}

int main(int argc, char *argv[])
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(helper_attaches_running_mstpd, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(helper_starts_mstpd, setup, teardown),
        cmocka_unit_test_setup_teardown(helper_bridge_created_with_stp, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(helper_restart, setup, teardown),
    };

    if (!sys_init_host() && sys_install_helper(write_hook()))
        fprintf(stderr, "cannot install the helper\n");
    return cmocka_run_group_tests(tests, NULL, NULL);
}
