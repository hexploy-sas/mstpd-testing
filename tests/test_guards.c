/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Port protection features: BPDU guard, root guard (restrictedRole,
 * 802.1Q-2005 13.25.14) and BPDU filter. One bridge, two ports without peer;
 * p1 has the feature, p2 is the reference; BPDUs injected.
 */

#include <stdio.h>
#include <linux/if_ether.h>
#include <linux/if_bridge.h>
#include <asm/byteorder.h>

#include "mstp.h"
#include "common.h"
#include "stp_params.h"

#define START_TICKS (MIGRATE_TIME + 1)
#define OBSERVE_TICKS 10
#define SUPERIOR_PRIO BRIDGE_PRIO_VALUE(1)
#define SUPERIOR_MAC 0x200000000009ull
#define DESIGNATED_FLAGS BPDU_FLAGS_ROLE_SET(encodedRoleDesignated)

static void setup(void **state, port_t *p[2], CIST_PortConfig *cfg)
{
    bridge_t *br;

    assert_int_equal(alloc_bridge_ports(state, &br, "br0", 0x200000000001,
                                        (port_t *(*)[])p, 2), 0);
    assert_int_equal(MSTP_IN_set_cist_port_config(p[0], cfg), 0);
    MSTP_IN_set_bridge_enable(br, true);
    set_port_state(p[0], true, SPEED_1000, DUPLEX_FULL);
    set_port_state(p[1], true, SPEED_1000, DUPLEX_FULL);
    test_ticks(state, START_TICKS);
}

static void rx_superior(port_t *p)
{
    bpdu_t b;

    make_bpdu(&b, protoRSTP, bpduTypeRST, SUPERIOR_PRIO, SUPERIOR_MAC,
              DESIGNATED_FLAGS);
    port_rx_bpdu(p, &b, RST_BPDU_SIZE);
}

static bool own_root(port_t *p)
{
    CIST_BridgeStatus s;

    MSTP_IN_get_cist_bridge_status(p->bridge, &s);
    return s.designated_root.u == s.bridge_id.u;
}

static void port_status(port_t *p, CIST_PortStatus *s)
{
    MSTP_IN_get_cist_port_status(p, s);
}

/* BPDU guard: BPDU received -> error flag, port shut down once, BPDU not
 * processed; the flag clears at the next link up */
static void bpdu_guard(void **state)
{
    CIST_PortConfig cfg = {
        .bpdu_guard_port = true,
        .set_bpdu_guard_port = true,
        .admin_edge_port = true,
        .set_admin_edge_port = true,
    };
    port_t *p[2];
    CIST_PortStatus s;

    setup(state, p, &cfg);
    port_status(p[0], &s);
    assert_false(s.bpdu_guard_error);

    rx_superior(p[0]);
    port_status(p[0], &s);
    assert_true(s.bpdu_guard_error);
    assert_int_equal(port_num_shutdown(p[0]), 1);
    assert_true(own_root(p[0]));

    set_port_state(p[0], false, SPEED_1000, DUPLEX_FULL);
    set_port_state(p[0], true, SPEED_1000, DUPLEX_FULL);
    port_status(p[0], &s);
    assert_false(s.bpdu_guard_error);

    /* reference port: BPDU processed, no shutdown */
    rx_superior(p[1]);
    assert_int_equal(port_num_shutdown(p[1]), 0);
    assert_false(own_root(p[1]));
}

/* Root guard: superior BPDU on a restricted port -> not root port, bridge
 * stays root; the same BPDU on the reference port makes it root port */
static void root_guard(void **state)
{
    CIST_PortConfig cfg = {
        .restricted_role = true,
        .set_restricted_role = true,
    };
    port_t *p[2];
    CIST_PortStatus s;

    setup(state, p, &cfg);

    rx_superior(p[0]);
    test_ticks(state, 1);
    port_status(p[0], &s);
    assert_int_equal(s.role, roleAlternate);
    assert_int_not_equal(s.state, BR_STATE_FORWARDING);
    assert_true(own_root(p[0]));
    port_status(p[1], &s);
    assert_int_equal(s.role, roleDesignated);

    rx_superior(p[1]);
    test_ticks(state, 1);
    port_status(p[1], &s);
    assert_int_equal(s.role, roleRoot);
    assert_false(own_root(p[1]));
}

/* BPDU filter: no BPDU sent; received BPDUs dropped and counted */
static void bpdu_filter(void **state)
{
    CIST_PortConfig cfg = {
        .bpdu_filter_port = true,
        .set_bpdu_filter_port = true,
    };
    port_t *p[2];
    CIST_PortStatus s;

    setup(state, p, &cfg);
    test_ticks(state, OBSERVE_TICKS);
    assert_int_equal(port_num_tx_bpdu(p[0]), 0);
    assert_true(port_num_tx_bpdu(p[1]) > 0);

    rx_superior(p[0]);
    rx_superior(p[0]);
    port_status(p[0], &s);
    assert_int_equal(s.num_rx_bpdu_filtered, 2);
    assert_true(own_root(p[0]));
}

int main(int argc, char *argv[])
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(bpdu_guard, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(root_guard, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(bpdu_filter, prepare_test, teardown_test),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
