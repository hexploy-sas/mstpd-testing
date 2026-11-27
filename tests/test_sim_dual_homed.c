/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ftest-sim: end node dual-homed to two interconnected switches.
 *   nodep1 <-> sw1p1, nodep2 <-> sw2p1, sw1p2 <-> sw2p2
 * Bridge priorities: sw1 best, sw2 next, node lowest (PRIO_MAX). node has
 * the lowest MAC, so only its priority keeps it from becoming root.
 */

#include <stdio.h>
#include <linux/if_ether.h>
#include <linux/if_bridge.h>
#include <asm/byteorder.h>

#include "mstp.h"
#include "common.h"
#include "stp_params.h"

#define CONVERGENCE_TICKS 10
#define RAPID_TICKS 3
#define AGE_OUT_TICKS RCVD_INFO_TIME

/* bridge priorities, in units of 4096 */
#define PRIO_SW1 1
#define PRIO_SW2 2
#define PRIO_NODE PRIO_MAX
#define PRIO_ROGUE 0

struct topo {
    bridge_t *node, *sw1, *sw2;
    port_t *nodep[2], *sw1p[2], *sw2p[2];
};

static void tick(void **state, int n)
{
    while (n--)
        test_one_second(state);
}

static void setup_bridge(bridge_t *br, __u8 prio)
{
    CIST_BridgeConfig cfg = {
        .protocol_version = protoRSTP,
        .set_protocol_version = true,
    };

    assert_int_equal(MSTP_IN_set_cist_bridge_config(br, &cfg), 0);
    assert_int_equal(MSTP_IN_set_msti_bridge_config(GET_CIST_TREE(br), prio), 0);
}

static void set_root_guard(port_t *p)
{
    CIST_PortConfig cfg = {
        .restricted_role = true,
        .set_restricted_role = true,
    };

    assert_int_equal(MSTP_IN_set_cist_port_config(p, &cfg), 0);
}

static void build(void **state, struct topo *t, bool root_guard)
{
    assert_int_equal(alloc_bridge_ports(state, &t->node, "node", 0x200000000001,
                                        &t->nodep, 2), 0);
    assert_int_equal(alloc_bridge_ports(state, &t->sw1, "sw1", 0x200000000002,
                                        &t->sw1p, 2), 0);
    assert_int_equal(alloc_bridge_ports(state, &t->sw2, "sw2", 0x200000000003,
                                        &t->sw2p, 2), 0);
    setup_bridge(t->node, PRIO_NODE);
    setup_bridge(t->sw1, PRIO_SW1);
    setup_bridge(t->sw2, PRIO_SW2);

    if (root_guard) {
        set_root_guard(t->sw1p[0]);
        set_root_guard(t->sw2p[0]);
    }

    link_ports(t->nodep[0], t->sw1p[0]);
    link_ports(t->nodep[1], t->sw2p[0]);
    link_ports(t->sw1p[1], t->sw2p[1]);

    MSTP_IN_set_bridge_enable(t->node, true);
    MSTP_IN_set_bridge_enable(t->sw1, true);
    MSTP_IN_set_bridge_enable(t->sw2, true);

    set_port_state(t->nodep[0], true, SPEED_1000, DUPLEX_FULL);
    set_port_state(t->nodep[1], true, SPEED_1000, DUPLEX_FULL);
    set_port_state(t->sw1p[1], true, SPEED_1000, DUPLEX_FULL);
}

static void assert_port(port_t *p, port_role_t role, bool forwarding)
{
    CIST_PortStatus s;

    MSTP_IN_get_cist_port_status(p, &s);
    assert_int_equal(s.role, role);
    if (forwarding)
        assert_int_equal(s.state, BR_STATE_FORWARDING);
    else
        assert_int_not_equal(s.state, BR_STATE_FORWARDING);
}

static void assert_root_is_sw1(struct topo *t, bool node_too)
{
    CIST_BridgeStatus sw1, b;

    MSTP_IN_get_cist_bridge_status(t->sw1, &sw1);
    assert_uint_equal(sw1.designated_root.u, sw1.bridge_id.u);
    MSTP_IN_get_cist_bridge_status(t->sw2, &b);
    assert_uint_equal(b.designated_root.u, sw1.bridge_id.u);
    if (node_too) {
        MSTP_IN_get_cist_bridge_status(t->node, &b);
        assert_uint_equal(b.designated_root.u, sw1.bridge_id.u);
    }
}

/* nominal topology: node never root, one node port alternate */
static void assert_nominal(struct topo *t)
{
    assert_root_is_sw1(t, true);
    assert_port(t->nodep[0], roleRoot, true);
    assert_port(t->nodep[1], roleAlternate, false);
    assert_port(t->sw1p[0], roleDesignated, true);
    assert_port(t->sw2p[0], roleDesignated, true);
    assert_port(t->sw2p[1], roleRoot, true);
}

/* End node dual-homed to 2 interconnected switches: node never root, 1 node
 * port alternate, failover within RAPID_TICKS */
static void dual_homed_failover(void **state)
{
    struct topo t;

    build(state, &t, false);
    tick(state, CONVERGENCE_TICKS);
    assert_nominal(&t);

    /* cut node <-> sw1 */
    set_port_state(t.nodep[0], false, SPEED_1000, DUPLEX_FULL);
    tick(state, RAPID_TICKS);

    assert_root_is_sw1(&t, true);
    assert_port(t.nodep[1], roleRoot, true);
    assert_port(t.sw2p[0], roleDesignated, true);

    /* restore: back to the nominal topology */
    set_port_state(t.nodep[0], true, SPEED_1000, DUPLEX_FULL);
    tick(state, CONVERGENCE_TICKS);
    assert_nominal(&t);
}

/* Root guard on the switch ports facing the end node, rogue superior BPDU
 * from the node (priority 0): root unchanged, guarded ports discarding while
 * superior info is received, recovery once it stops */
static void dual_homed_root_guard(void **state)
{
    struct topo t;

    build(state, &t, true);
    tick(state, CONVERGENCE_TICKS);
    assert_nominal(&t);

    assert_int_equal(MSTP_IN_set_msti_bridge_config(GET_CIST_TREE(t.node),
                                                    PRIO_ROGUE), 0);
    tick(state, RAPID_TICKS);

    assert_root_is_sw1(&t, false);
    assert_port(t.sw1p[0], roleAlternate, false);
    assert_port(t.sw2p[0], roleAlternate, false);
    assert_port(t.sw2p[1], roleRoot, true);

    /* superior info persists: still discarding several hello times later */
    tick(state, AGE_OUT_TICKS);
    assert_root_is_sw1(&t, false);
    assert_port(t.sw1p[0], roleAlternate, false);
    assert_port(t.sw2p[0], roleAlternate, false);

    /* rogue configuration removed */
    assert_int_equal(MSTP_IN_set_msti_bridge_config(GET_CIST_TREE(t.node),
                                                    PRIO_NODE), 0);
    tick(state, AGE_OUT_TICKS + RAPID_TICKS);
    assert_nominal(&t);
}

int main(int argc, char *argv[])
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(dual_homed_failover, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(dual_homed_root_guard, prepare_test, teardown_test),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
