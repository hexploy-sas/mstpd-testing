/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Topology Change state machine (802.1D-2004 17.31): a TC received on a
 * port flushes the other non-edge ports only; restrictedTcn (802.1Q-2005
 * 13.25.15) stops the propagation.
 *   br0p1: root port, superior BPDUs injected
 *   br0p2 <-> br2p1, br0p3 <-> br3p1: designated, non-edge
 *   br0p4: admin edge, no peer
 */

#include <stdio.h>
#include <linux/if_ether.h>
#include <linux/if_bridge.h>
#include <asm/byteorder.h>

#include "mstp.h"
#include "common.h"
#include "stp_params.h"

#define SETTLE_TICKS 20
#define SUPERIOR_PRIO BRIDGE_PRIO_VALUE(1)
#define SUPERIOR_MAC 0x200000000009ull
#define ROOT_PORT_FLAGS (BPDU_FLAGS_ROLE_SET(encodedRoleDesignated) \
                         | (1 << offsetLearnig) | (1 << offsetForwarding))
#define TC_FLAG (1 << offsetTc)

struct topo {
    bridge_t *br0, *br2, *br3;
    port_t *p0[4], *p2[1], *p3[1];
};

static void rx_root_bpdu(port_t *p, bool tc)
{
    bpdu_t b;

    make_bpdu(&b, protoRSTP, bpduTypeRST, SUPERIOR_PRIO, SUPERIOR_MAC,
              ROOT_PORT_FLAGS | (tc ? TC_FLAG : 0));
    port_rx_bpdu(p, &b, RST_BPDU_SIZE);
}

/* the injected root sends a BPDU every tick */
static void ticks_with_root(void **state, struct topo *t, int n)
{
    while (n--) {
        rx_root_bpdu(t->p0[0], false);
        test_one_second(state);
    }
}

static void build(void **state, struct topo *t, bool restricted_tcn)
{
    CIST_PortConfig edge = {
        .admin_edge_port = true,
        .set_admin_edge_port = true,
    };
    CIST_PortConfig rtcn = {
        .restricted_tcn = restricted_tcn,
        .set_restricted_tcn = true,
    };
    CIST_PortStatus s;
    int i;

    assert_int_equal(alloc_bridge_ports(state, &t->br0, "br0", 0x200000000001,
                                        &t->p0, 4), 0);
    assert_int_equal(alloc_bridge_ports(state, &t->br2, "br2", 0x200000000002,
                                        &t->p2, 1), 0);
    assert_int_equal(alloc_bridge_ports(state, &t->br3, "br3", 0x200000000003,
                                        &t->p3, 1), 0);
    assert_int_equal(MSTP_IN_set_cist_port_config(t->p0[0], &rtcn), 0);
    assert_int_equal(MSTP_IN_set_cist_port_config(t->p0[3], &edge), 0);
    link_ports(t->p0[1], t->p2[0]);
    link_ports(t->p0[2], t->p3[0]);
    MSTP_IN_set_bridge_enable(t->br0, true);
    MSTP_IN_set_bridge_enable(t->br2, true);
    MSTP_IN_set_bridge_enable(t->br3, true);
    for (i = 0; i < 4; i++)
        set_port_state(t->p0[i], true, SPEED_1000, DUPLEX_FULL);
    ticks_with_root(state, t, SETTLE_TICKS);

    MSTP_IN_get_cist_port_status(t->p0[0], &s);
    assert_int_equal(s.role, roleRoot);
    assert_int_equal(s.state, BR_STATE_FORWARDING);
    for (i = 1; i < 3; i++) {
        MSTP_IN_get_cist_port_status(t->p0[i], &s);
        assert_int_equal(s.role, roleDesignated);
        assert_int_equal(s.state, BR_STATE_FORWARDING);
        assert_false(s.oper_edge_port);
    }
    MSTP_IN_get_cist_port_status(t->p0[3], &s);
    assert_true(s.oper_edge_port);
    assert_int_equal(s.state, BR_STATE_FORWARDING);
}

static void flush_counts(struct topo *t, int n[4])
{
    int i;

    for (i = 0; i < 4; i++)
        n[i] = port_num_flush(t->p0[i]);
}

/* TC received on the root port: the other non-edge ports are flushed; the
 * receiving port and the edge port are not */
static void tc_flushes_other_non_edge_ports(void **state)
{
    struct topo t;
    int before[4], after[4];

    build(state, &t, false);
    flush_counts(&t, before);

    rx_root_bpdu(t.p0[0], true);
    flush_counts(&t, after);

    assert_int_equal(after[0], before[0]);
    assert_true(after[1] > before[1]);
    assert_true(after[2] > before[2]);
    assert_int_equal(after[3], before[3]);
}

/* restrictedTcn on the root port: a received TC is not propagated */
static void tcn_guard_stops_propagation(void **state)
{
    struct topo t;
    int before[4], after[4], i;

    build(state, &t, true);
    flush_counts(&t, before);

    rx_root_bpdu(t.p0[0], true);
    ticks_with_root(state, &t, 2);
    flush_counts(&t, after);

    for (i = 0; i < 4; i++)
        assert_int_equal(after[i], before[i]);
}

int main(int argc, char *argv[])
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(tc_flushes_other_non_edge_ports, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(tcn_guard_stops_propagation, prepare_test, teardown_test),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
