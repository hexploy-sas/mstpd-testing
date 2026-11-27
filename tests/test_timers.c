/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Received information ageing (802.1D-2004 17.21.23 updtRcvdInfoWhile, Port
 * Information state machine) and Transmit Hold Count (17.13.12, Port
 * Transmit state machine). One bridge; BPDUs injected on p1.
 */

#include <stdio.h>
#include <linux/if_ether.h>
#include <linux/if_bridge.h>
#include <asm/byteorder.h>

#include "mstp.h"
#include "common.h"
#include "stp_params.h"

#define START_TICKS (MIGRATE_TIME + 1)
#define BURST 10
#define SUPERIOR_PRIO BRIDGE_PRIO_VALUE(1)
/* root path cost alternating with 0 to change the designated info */
#define ALT_COST 1000
/* a Transmit Hold Count below the default */
#define TX_HOLD_LOW 2
#define SUPERIOR_MAC 0x200000000009ull
#define DESIGNATED_FLAGS (BPDU_FLAGS_ROLE_SET(encodedRoleDesignated) \
                          | (1 << offsetLearnig) | (1 << offsetForwarding))

static void setup(void **state, port_t *p[2])
{
    bridge_t *br;

    assert_int_equal(alloc_bridge_ports(state, &br, "br0", 0x200000000001,
                                        (port_t *(*)[])p, 2), 0);
    MSTP_IN_set_bridge_enable(br, true);
    set_port_state(p[0], true, SPEED_1000, DUPLEX_FULL);
    set_port_state(p[1], true, SPEED_1000, DUPLEX_FULL);
    test_ticks(state, START_TICKS);
}

static void rx_superior(port_t *p, __u8 msg_age, __u32 cost)
{
    bpdu_t b;

    make_bpdu(&b, protoRSTP, bpduTypeRST, SUPERIOR_PRIO, SUPERIOR_MAC,
              DESIGNATED_FLAGS);
    b.MessageAge[0] = msg_age;
    b.cistExtRootPathCost = __cpu_to_be32(cost);
    port_rx_bpdu(p, &b, RST_BPDU_SIZE);
}

static bool own_root(port_t *p)
{
    CIST_BridgeStatus s;

    MSTP_IN_get_cist_bridge_status(p->bridge, &s);
    return s.designated_root.u == s.bridge_id.u;
}

/* Received info kept for 3 x Hello Time, then aged out: the bridge
 * re-elects itself root */
static void info_aged_out(void **state)
{
    port_t *p[2];
    CIST_PortStatus s;

    setup(state, p);
    rx_superior(p[0], 0, 0);
    assert_false(own_root(p[0]));

    test_ticks(state, RCVD_INFO_TIME - 1);
    assert_false(own_root(p[0]));
    MSTP_IN_get_cist_port_status(p[0], &s);
    assert_int_equal(s.role, roleRoot);

    test_ticks(state, 1);
    assert_true(own_root(p[0]));
    MSTP_IN_get_cist_port_status(p[0], &s);
    assert_int_equal(s.role, roleDesignated);
}

/* Info with Message Age + 1 > Max Age is aged out at once */
static void info_message_age_limit(void **state)
{
    port_t *p[2];

    setup(state, p);
    rx_superior(p[0], MAX_AGE_DEFAULT, 0);
    test_ticks(state, 1);
    assert_true(own_root(p[0]));

    rx_superior(p[0], MAX_AGE_DEFAULT - 1, 0);
    test_ticks(state, 1);
    assert_false(own_root(p[0]));
}

/* A burst of changes sends at most Transmit Hold Count BPDUs per port
 * before the next tick */
static void check_tx_hold(void **state, port_t *p[2], unsigned int hold)
{
    CIST_BridgeConfig cfg = {
        .tx_hold_count = hold,
        .set_tx_hold_count = true,
    };
    int before, i;

    /* setting the count resets txCount and sends pending info at once */
    before = port_num_tx_bpdu(p[1]);
    assert_int_equal(MSTP_IN_set_cist_bridge_config(p[0]->bridge, &cfg), 0);
    /* each root path cost change gives new designated info on p2 */
    for (i = 0; i < BURST; i++)
        rx_superior(p[0], 0, (i % 2) ? 0 : ALT_COST);
    assert_int_equal(port_num_tx_bpdu(p[1]) - before, hold);
}

static void tx_hold_count(void **state)
{
    port_t *p[2];

    setup(state, p);
    rx_superior(p[0], 0, 0);
    test_ticks(state, 1);

    check_tx_hold(state, p, TX_HOLD_LOW);
    check_tx_hold(state, p, TX_HOLD_DEFAULT);
}

int main(int argc, char *argv[])
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(info_aged_out, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(info_message_age_limit, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(tx_hold_count, prepare_test, teardown_test),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
