/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Bridge Detection state machine (802.1D-2004 17.25): admin edge and auto
 * edge. One bridge, one port without peer; BPDUs injected.
 */

#include <stdio.h>
#include <linux/if_ether.h>
#include <linux/if_bridge.h>
#include <asm/byteorder.h>

#include "mstp.h"
#include "common.h"
#include "stp_params.h"

#define DESIGNATED_FLAGS BPDU_FLAGS_ROLE_SET(encodedRoleDesignated)

static port_t *setup(void **state, bool admin_edge, bool auto_edge)
{
    bridge_t *br;
    port_t *p[1];
    CIST_PortConfig cfg = {
        .admin_edge_port = admin_edge,
        .set_admin_edge_port = true,
        .auto_edge_port = auto_edge,
        .set_auto_edge_port = true,
    };

    assert_int_equal(alloc_bridge_ports(state, &br, "br0", 0x200000000001,
                                        &p, 1), 0);
    assert_int_equal(MSTP_IN_set_cist_port_config(p[0], &cfg), 0);
    MSTP_IN_set_bridge_enable(br, true);

    return p[0];
}

static void port_status(port_t *p, CIST_PortStatus *s)
{
    MSTP_IN_get_cist_port_status(p, s);
}

/* inferior RST BPDU from a designated neighbour: the port stays designated */
static void rx_neighbour_bpdu(port_t *p)
{
    bpdu_t b;

    make_bpdu(&b, protoRSTP, bpduTypeRST, BRIDGE_PRIO_VALUE(PRIO_MAX),
              0x200000000009ull,
              DESIGNATED_FLAGS);
    port_rx_bpdu(p, &b, RST_BPDU_SIZE);
}

/* Admin edge: oper edge and forwarding at link up, without waiting */
static void admin_edge_forwards_at_once(void **state)
{
    port_t *p = setup(state, true, false);
    CIST_PortStatus s;

    set_port_state(p, true, SPEED_1000, DUPLEX_FULL);
    port_status(p, &s);
    assert_true(s.oper_edge_port);
    assert_int_equal(s.role, roleDesignated);
    assert_int_equal(s.state, BR_STATE_FORWARDING);
}

/* Admin edge: a received BPDU clears oper edge; admin edge is kept and
 * applies again at the next link up */
static void admin_edge_lost_on_bpdu(void **state)
{
    port_t *p = setup(state, true, false);
    CIST_PortStatus s;

    set_port_state(p, true, SPEED_1000, DUPLEX_FULL);
    test_ticks(state, 1);
    rx_neighbour_bpdu(p);

    port_status(p, &s);
    assert_false(s.oper_edge_port);
    assert_true(s.admin_edge_port);

    set_port_state(p, false, SPEED_1000, DUPLEX_FULL);
    test_ticks(state, 1);
    set_port_state(p, true, SPEED_1000, DUPLEX_FULL);
    port_status(p, &s);
    assert_true(s.oper_edge_port);
    assert_int_equal(s.state, BR_STATE_FORWARDING);
}

/* Auto edge: no BPDU during edgeDelayWhile (Migrate Time) -> oper edge and
 * forwarding, well before 2 x Forward Delay */
static void auto_edge_no_bpdu(void **state)
{
    port_t *p = setup(state, false, true);
    CIST_PortStatus s;

    set_port_state(p, true, SPEED_1000, DUPLEX_FULL);
    test_ticks(state, MIGRATE_TIME - 1);
    port_status(p, &s);
    assert_false(s.oper_edge_port);
    assert_int_not_equal(s.state, BR_STATE_FORWARDING);

    /* edgeDelayWhile (Migrate Time) expires */
    test_ticks(state, 2);
    port_status(p, &s);
    assert_true(s.oper_edge_port);
    assert_int_equal(s.state, BR_STATE_FORWARDING);

    /* a received BPDU clears oper edge */
    rx_neighbour_bpdu(p);
    port_status(p, &s);
    assert_false(s.oper_edge_port);
}

/* Auto edge off, no admin edge, no agreement: never edge. fdWhile starts at
 * Max Age (DISABLED_PORT), then learning lasts forwardDelay: Hello Time with
 * RSTP, Forward Delay with STP (17.20.5) */
static void check_no_edge_timers(void **state, protocol_version_t proto,
                                 int learn_ticks)
{
    port_t *p = setup(state, false, false);
    CIST_BridgeConfig cfg = {
        .protocol_version = proto,
        .set_protocol_version = true,
    };
    CIST_PortStatus s;

    assert_int_equal(MSTP_IN_set_cist_bridge_config(p->bridge, &cfg), 0);
    set_port_state(p, true, SPEED_1000, DUPLEX_FULL);
    test_ticks(state, MAX_AGE_DEFAULT - 1);
    port_status(p, &s);
    assert_int_equal(s.state, BR_STATE_BLOCKING);

    test_ticks(state, 1);
    port_status(p, &s);
    assert_int_equal(s.state, BR_STATE_LEARNING);

    test_ticks(state, learn_ticks - 1);
    port_status(p, &s);
    assert_int_equal(s.state, BR_STATE_LEARNING);

    test_ticks(state, 1);
    port_status(p, &s);
    assert_false(s.oper_edge_port);
    assert_int_equal(s.state, BR_STATE_FORWARDING);
}

static void no_edge_rstp_timers(void **state)
{
    check_no_edge_timers(state, protoRSTP, HELLO_TIME);
}

static void no_edge_stp_timers(void **state)
{
    check_no_edge_timers(state, protoSTP, FWD_DELAY_DEFAULT);
}

int main(int argc, char *argv[])
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(admin_edge_forwards_at_once, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(admin_edge_lost_on_bpdu, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(auto_edge_no_bpdu, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(no_edge_rstp_timers, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(no_edge_stp_timers, prepare_test, teardown_test),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
