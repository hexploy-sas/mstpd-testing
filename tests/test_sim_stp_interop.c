/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ftest-sim: RSTP bridges next to a legacy STP bridge.
 * The legacy bridge is an mstpd bridge forced to STP whose ports drop RST
 * BPDUs, as an 802.1D-1998 bridge does.
 * Timers: shortest valid pair (MAX_AGE_MIN, FWD_DELAY_MIN).
 */

#include <stdio.h>
#include <linux/if_ether.h>
#include <linux/if_bridge.h>
#include <asm/byteorder.h>

#include "mstp.h"
#include "common.h"
#include "stp_params.h"

#define RAPID_TICKS 3
#define FWD_DELAY FWD_DELAY_MIN
#define MAX_AGE MAX_AGE_MIN
#define SETTLE_TICKS 20
#define OBSERVE_TICKS 20

static void tick(void **state, int n)
{
    while (n--)
        test_one_second(state);
}

static void setup_bridge(bridge_t *br, protocol_version_t proto)
{
    CIST_BridgeConfig cfg = {
        .protocol_version = proto,
        .set_protocol_version = true,
        .bridge_max_age = MAX_AGE,
        .set_bridge_max_age = true,
        .bridge_forward_delay = FWD_DELAY,
        .set_bridge_forward_delay = true,
    };

    assert_int_equal(MSTP_IN_set_cist_bridge_config(br, &cfg), 0);
}

static void port_status(port_t *p, CIST_PortStatus *s)
{
    MSTP_IN_get_cist_port_status(p, s);
}

/* Mixed STP/RSTP neighbour, RSTP root: legacy timers on the STP segment only.
 *   rstp root br0: p1 <-> legacy br1 p1, p2 <-> rstp br2 p1 */
static void stp_segment_legacy_timers(void **state)
{
    bridge_t *br0, *br1, *br2;
    port_t *p0[2], *p1[1], *p2[1];
    CIST_PortStatus s;
    int t;

    assert_int_equal(alloc_bridge_ports(state, &br0, "br0", 0x200000000001,
                                        &p0, 2), 0);
    assert_int_equal(alloc_bridge_ports(state, &br1, "br1", 0x200000000002,
                                        &p1, 1), 0);
    assert_int_equal(alloc_bridge_ports(state, &br2, "br2", 0x200000000003,
                                        &p2, 1), 0);
    setup_bridge(br0, protoRSTP);
    setup_bridge(br1, protoSTP);
    setup_bridge(br2, protoRSTP);
    port_set_drop_rx_rst(p1[0], true);

    link_ports(p0[0], p1[0]);
    link_ports(p0[1], p2[0]);
    MSTP_IN_set_bridge_enable(br0, true);
    MSTP_IN_set_bridge_enable(br1, true);
    MSTP_IN_set_bridge_enable(br2, true);
    set_port_state(p0[0], true, SPEED_1000, DUPLEX_FULL);
    set_port_state(p0[1], true, SPEED_1000, DUPLEX_FULL);

    tick(state, RAPID_TICKS);

    /* RSTP segment: rapid transition */
    port_status(p0[1], &s);
    assert_true(s.sendRSTP);
    assert_int_equal(s.role, roleDesignated);
    assert_int_equal(s.state, BR_STATE_FORWARDING);

    /* STP segment: no rapid transition, no auto edge */
    port_status(p0[0], &s);
    assert_false(s.oper_edge_port);
    assert_int_equal(s.role, roleDesignated);
    assert_int_not_equal(s.state, BR_STATE_FORWARDING);

    /* forwarding only after listening + learning (2 x Forward Delay) */
    for (t = RAPID_TICKS; t < 2 * FWD_DELAY - 1; t++) {
        tick(state, 1);
        port_status(p0[0], &s);
        assert_false(s.oper_edge_port);
        assert_int_not_equal(s.state, BR_STATE_FORWARDING);
    }
    tick(state, SETTLE_TICKS);

    /* the port migrated to STP BPDUs (after Migrate Time) and forwards */
    port_status(p0[0], &s);
    assert_false(s.sendRSTP);
    assert_int_equal(s.role, roleDesignated);
    assert_int_equal(s.state, BR_STATE_FORWARDING);

    /* the legacy bridge accepted br0 as root */
    port_status(p1[0], &s);
    assert_int_equal(s.role, roleRoot);

    /* the RSTP segment never fell back to STP */
    port_status(p0[1], &s);
    assert_true(s.sendRSTP);
    port_status(p2[0], &s);
    assert_true(s.sendRSTP);
}

/* Mixed STP/RSTP neighbour, legacy STP root (#5): once converged, the RSTP
 * bridge does not send a TCN every hello time.
 *   legacy root br0: p1 <-> rstp br1 p1; br1 p2 <-> rstp br2 p1 */
static void stp_root_no_periodic_tcn(void **state)
{
    bridge_t *br0, *br1, *br2;
    port_t *p0[1], *p1[2], *p2[1];
    CIST_BridgeStatus b0, b;
    CIST_PortStatus s;
    int tcn_before;
    unsigned int tc_count_before;

    assert_int_equal(alloc_bridge_ports(state, &br0, "br0", 0x200000000001,
                                        &p0, 1), 0);
    assert_int_equal(alloc_bridge_ports(state, &br1, "br1", 0x200000000002,
                                        &p1, 2), 0);
    assert_int_equal(alloc_bridge_ports(state, &br2, "br2", 0x200000000003,
                                        &p2, 1), 0);
    setup_bridge(br0, protoSTP);
    setup_bridge(br1, protoRSTP);
    setup_bridge(br2, protoRSTP);
    port_set_drop_rx_rst(p0[0], true);

    link_ports(p0[0], p1[0]);
    link_ports(p1[1], p2[0]);
    MSTP_IN_set_bridge_enable(br0, true);
    MSTP_IN_set_bridge_enable(br1, true);
    MSTP_IN_set_bridge_enable(br2, true);
    set_port_state(p0[0], true, SPEED_1000, DUPLEX_FULL);
    set_port_state(p1[1], true, SPEED_1000, DUPLEX_FULL);

    tick(state, SETTLE_TICKS);

    MSTP_IN_get_cist_bridge_status(br0, &b0);
    MSTP_IN_get_cist_bridge_status(br1, &b);
    assert_uint_equal(b.designated_root.u, b0.bridge_id.u);
    MSTP_IN_get_cist_bridge_status(br2, &b);
    assert_uint_equal(b.designated_root.u, b0.bridge_id.u);

    port_status(p1[0], &s);
    assert_int_equal(s.role, roleRoot);
    assert_false(s.sendRSTP);
    assert_int_equal(s.state, BR_STATE_FORWARDING);
    tcn_before = port_num_tx_tcn_bpdu(p1[0]);
    MSTP_IN_get_cist_bridge_status(br0, &b0);
    tc_count_before = b0.topology_change_count;

    tick(state, OBSERVE_TICKS);

    assert_int_equal(port_num_tx_tcn_bpdu(p1[0]), tcn_before);
    MSTP_IN_get_cist_bridge_status(br0, &b0);
    assert_uint_equal(b0.topology_change_count, tc_count_before);

    /* a real topology change (br1 <-> br2 link back to forwarding) sends
     * TCNs to the legacy root until acknowledged, then they stop */
    set_port_state(p1[1], false, SPEED_1000, DUPLEX_FULL);
    tick(state, 1);
    set_port_state(p1[1], true, SPEED_1000, DUPLEX_FULL);
    tick(state, SETTLE_TICKS);

    assert_true(port_num_tx_tcn_bpdu(p1[0]) > tcn_before);
    MSTP_IN_get_cist_bridge_status(br0, &b0);
    assert_true(b0.topology_change_count > tc_count_before);

    tcn_before = port_num_tx_tcn_bpdu(p1[0]);
    tick(state, OBSERVE_TICKS);
    assert_int_equal(port_num_tx_tcn_bpdu(p1[0]), tcn_before);
}

int main(int argc, char *argv[])
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(stp_segment_legacy_timers, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(stp_root_no_periodic_tcn, prepare_test, teardown_test),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
