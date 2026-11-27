/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ftest-sys: ring of 3 real mstpd instances, one per network namespace.
 *   dut0 e1 <-> dut1 e1, dut1 e2 <-> dut2 e1, dut2 e2 <-> dut0 e2
 * Bridge MACs 02:00:00:00:00:01..03, default priority: dut0 is root and
 * dut2 e1 (facing dut1) is the alternate port.
 */

#include <stdio.h>
#include <string.h>
#include <linux/if_bridge.h>

#include "sys.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define NUM_DUTS 3
#define CONVERGE_MS 15000
#define FAILOVER_MS 10000

struct ring {
    struct dut d[NUM_DUTS];
};

static struct ring ring;

static int setup(void **state)
{
    struct ring *r = &ring;
    int i;

    if (sys_skip_reason())
        return 0;
    memset(r, 0, sizeof(*r));
    for (i = 0; i < NUM_DUTS; i++)
        r->d[i].netns = -1;
    assert_int_equal(dut_create(&r->d[0], "dut0", 1), 0);
    assert_int_equal(dut_create(&r->d[1], "dut1", 2), 0);
    assert_int_equal(dut_create(&r->d[2], "dut2", 3), 0);
    assert_int_equal(dut_link(&r->d[0], "e1", &r->d[1], "e1"), 0);
    assert_int_equal(dut_link(&r->d[1], "e2", &r->d[2], "e1"), 0);
    assert_int_equal(dut_link(&r->d[2], "e2", &r->d[0], "e2"), 0);
    for (i = 0; i < NUM_DUTS; i++)
        assert_int_equal(dut_start(&r->d[i]), 0);
    *state = r;
    return 0;
}

static int teardown(void **state)
{
    int i;

    for (i = 0; i < NUM_DUTS; i++)
        dut_destroy(&ring.d[i]);
    return 0;
}

struct port_expect {
    int dut;
    const char *port;
    const char *role;
    int state;
};

/* nominal ring */
static const struct port_expect nominal[] = {
    { 0, "e1", "Designated", BR_STATE_FORWARDING },
    { 0, "e2", "Designated", BR_STATE_FORWARDING },
    { 1, "e1", "Root",       BR_STATE_FORWARDING },
    { 1, "e2", "Designated", BR_STATE_FORWARDING },
    { 2, "e1", "Alternate",  BR_STATE_BLOCKING },
    { 2, "e2", "Root",       BR_STATE_FORWARDING },
};

/* after the dut2 e2 <-> dut0 e2 link is cut */
static const struct port_expect cut[] = {
    { 0, "e1", "Designated", BR_STATE_FORWARDING },
    { 1, "e1", "Root",       BR_STATE_FORWARDING },
    { 1, "e2", "Designated", BR_STATE_FORWARDING },
    { 2, "e1", "Root",       BR_STATE_FORWARDING },
};

struct expect_set {
    struct ring *r;
    const struct port_expect *e;
    int n;
};

/* kernel states are the ground truth for convergence */
static bool states_match(void *arg)
{
    struct expect_set *s = arg;
    int i;

    for (i = 0; i < s->n; i++)
        if (dut_port_state(&s->r->d[s->e[i].dut], s->e[i].port)
            != s->e[i].state)
            return false;
    return true;
}

static void assert_roles(struct expect_set *s)
{
    char role[32];
    int i;

    for (i = 0; i < s->n; i++) {
        assert_int_equal(dut_port_role(&s->r->d[s->e[i].dut], s->e[i].port,
                                       role, sizeof(role)), 0);
        assert_string_equal(role, s->e[i].role);
    }
}

static void assert_root_is_dut0(struct ring *r)
{
    char id0[64], root[64], id[64];
    int i;

    assert_int_equal(dut_bridge_ids(&r->d[0], id0, root, sizeof(id0)), 0);
    assert_string_equal(root, id0);
    for (i = 1; i < NUM_DUTS; i++) {
        assert_int_equal(dut_bridge_ids(&r->d[i], id, root, sizeof(id)), 0);
        assert_string_equal(root, id0);
    }
}

static long converge(struct ring *r)
{
    struct expect_set s = { r, nominal, ARRAY_SIZE(nominal) };
    long ms = sys_wait(states_match, &s, CONVERGE_MS);

    assert_true(ms >= 0);
    assert_roles(&s);
    assert_root_is_dut0(r);
    return ms;
}

/* Ring of 3 real mstpd: same root everywhere, 2 root ports, 1 alternate
 * blocking (dut2 e1), kernel states match the roles */
static void ring_converges(void **state)
{
    long ms;

    sys_require();
    ms = converge(*state);
    print_message("converged in %ld ms\n", ms);
}

/* Root port link of dut2 cut: dut2 e1 becomes root port and forwards;
 * failover time logged */
static void ring_link_cut(void **state)
{
    struct ring *r = *state;
    struct expect_set s = { r, cut, ARRAY_SIZE(cut) };
    long ms;

    sys_require();
    converge(r);

    assert_int_equal(dut_set_link(&r->d[2], "e2", false), 0);
    ms = sys_wait(states_match, &s, FAILOVER_MS);
    assert_true(ms >= 0);
    assert_roles(&s);
    assert_root_is_dut0(r);
    print_message("failover in %ld ms\n", ms);
}

int main(int argc, char *argv[])
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(ring_converges, setup, teardown),
        cmocka_unit_test_setup_teardown(ring_link_cut, setup, teardown),
    };

    sys_init();
    return cmocka_run_group_tests(tests, NULL, NULL);
}
