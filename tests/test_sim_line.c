/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ftest-sim: network diameter bounded by Max Age.
 * Topology: line br0 -- br1 -- ... -- brN-1, br0 has the lowest bridge ID.
 *   bri p2 <-> bri+1 p1
 * The root sends Message Age 0 and each hop adds 1. Info received with
 * Message Age + 1 > Max Age is discarded, so a bridge more than Max Age hops
 * from the root does not accept it as root.
 */

#include <stdio.h>
#include <linux/if_ether.h>
#include <linux/if_bridge.h>
#include <asm/byteorder.h>

#include "mstp.h"
#include "common.h"
#include "stp_params.h"

#define MAX_LINE (MAX_AGE_DEFAULT + 2)
#define SETTLE_TICKS 30

struct line {
    int n;
    bridge_t *br[MAX_LINE];
    port_t *p[MAX_LINE][2];
};

static void tick(void **state, int n)
{
    while (n--)
        test_one_second(state);
}

static void build_line(void **state, struct line *l, int n, __u8 max_age)
{
    CIST_BridgeConfig cfg = {
        .protocol_version = protoRSTP,
        .set_protocol_version = true,
        .bridge_max_age = max_age,
        .set_bridge_max_age = true,
    };
    char name[IFNAMSIZ];
    int i;

    assert_true(n <= MAX_LINE);
    l->n = n;

    for (i = 0; i < n; i++) {
        snprintf(name, sizeof(name), "br%d", i);
        assert_int_equal(alloc_bridge_ports(state, &l->br[i], name,
                                            0x200000000001 + i, &l->p[i], 2), 0);
        assert_int_equal(MSTP_IN_set_cist_bridge_config(l->br[i], &cfg), 0);
    }

    for (i = 0; i + 1 < n; i++)
        link_ports(l->p[i][1], l->p[i + 1][0]);

    for (i = 0; i < n; i++)
        MSTP_IN_set_bridge_enable(l->br[i], true);

    for (i = 0; i + 1 < n; i++)
        set_port_state(l->p[i][1], true, SPEED_1000, DUPLEX_FULL);
}

/* number of bridges, from br0, that accept br0 as root */
static int count_root_accepted(struct line *l)
{
    CIST_BridgeStatus b0, b;
    int i;

    MSTP_IN_get_cist_bridge_status(l->br[0], &b0);
    for (i = 0; i < l->n; i++) {
        MSTP_IN_get_cist_bridge_status(l->br[i], &b);
        if (b.designated_root.u != b0.bridge_id.u)
            break;
    }
    return i;
}

/* line of max_age + 1 bridges: all accept the root;
 * line of max_age + 2 bridges: the last one does not */
static void check_diameter(void **state, __u8 max_age)
{
    struct line l;
    CIST_BridgeStatus b;
    int last = max_age + 1;

    build_line(state, &l, max_age + 1, max_age);
    tick(state, SETTLE_TICKS);
    assert_int_equal(count_root_accepted(&l), max_age + 1);

    teardown_test(state);
    assert_int_equal(prepare_test(state), 0);

    build_line(state, &l, max_age + 2, max_age);
    tick(state, SETTLE_TICKS);
    assert_int_equal(count_root_accepted(&l), max_age + 1);

    /* the bridge beyond the diameter sees itself as root */
    MSTP_IN_get_cist_bridge_status(l.br[last], &b);
    assert_uint_equal(b.designated_root.u, b.bridge_id.u);
}

/* Max Age at its minimum */
static void line_max_age_min(void **state)
{
    check_diameter(state, MAX_AGE_MIN);
}

/* Max Age at its default */
static void line_max_age_default(void **state)
{
    check_diameter(state, MAX_AGE_DEFAULT);
}

int main(int argc, char *argv[])
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(line_max_age_min, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(line_max_age_default, prepare_test, teardown_test),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
