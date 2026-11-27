/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * BPDU validation on receipt (802.1D-2004 9.3.4, 802.1Q-2005 14.4).
 * One bridge, one port. An accepted superior BPDU changes the root; a
 * dropped one changes nothing and does not count as a received TC.
 */

#include <stdio.h>
#include <linux/if_ether.h>
#include <linux/if_bridge.h>
#include <asm/byteorder.h>

#include "mstp.h"
#include "common.h"
#include "stp_params.h"

#define START_TICKS (MIGRATE_TIME + 1)
#define SUPERIOR_PRIO BRIDGE_PRIO_VALUE(1)
#define BPDU_TYPE_UNKNOWN 0x55
#define SUPERIOR_MAC 0x200000000009ull
#define TC_FLAG (1 << offsetTc)
#define DESIGNATED_FLAGS BPDU_FLAGS_ROLE_SET(encodedRoleDesignated)

static port_t *setup(void **state)
{
    bridge_t *br;
    port_t *p[1];

    assert_int_equal(alloc_bridge_ports(state, &br, "br0", 0x200000000001,
                                        &p, 1), 0);
    MSTP_IN_set_bridge_enable(br, true);
    set_port_state(p[0], true, SPEED_1000, DUPLEX_FULL);
    test_ticks(state, START_TICKS);

    return p[0];
}

static bool own_root(port_t *p)
{
    CIST_BridgeStatus s;

    MSTP_IN_get_cist_bridge_status(p->bridge, &s);
    return s.designated_root.u == s.bridge_id.u;
}

static unsigned int rx_tcn(port_t *p)
{
    CIST_PortStatus s;

    MSTP_IN_get_cist_port_status(p, &s);
    return s.num_rx_tcn;
}

static void assert_dropped(port_t *p, const bpdu_t *b, size_t len)
{
    unsigned int tcn = rx_tcn(p);

    port_rx_bpdu(p, b, len);
    assert_true(own_root(p));
    assert_int_equal(rx_tcn(p), tcn);
}

static void assert_accepted(port_t *p, const bpdu_t *b, size_t len)
{
    unsigned int tcn = rx_tcn(p);

    port_rx_bpdu(p, b, len);
    assert_false(own_root(p));
    assert_int_equal(rx_tcn(p), tcn + 1);
}

/* RST BPDU shorter than 36 octets dropped */
static void rx_rst_too_short(void **state)
{
    port_t *p = setup(state);
    bpdu_t b;

    make_bpdu(&b, protoRSTP, bpduTypeRST, SUPERIOR_PRIO, SUPERIOR_MAC,
              DESIGNATED_FLAGS | TC_FLAG);
    assert_dropped(p, &b, RST_BPDU_SIZE - 1);
    assert_accepted(p, &b, RST_BPDU_SIZE);
}

/* Config BPDU shorter than 35 octets dropped */
static void rx_config_too_short(void **state)
{
    port_t *p = setup(state);
    bpdu_t b;

    make_bpdu(&b, protoSTP, bpduTypeConfig, SUPERIOR_PRIO, SUPERIOR_MAC,
              TC_FLAG);
    assert_dropped(p, &b, CONFIG_BPDU_SIZE - 1);
    assert_accepted(p, &b, CONFIG_BPDU_SIZE);
}

/* TCN BPDU shorter than 4 octets dropped */
static void rx_tcn_too_short(void **state)
{
    port_t *p = setup(state);
    bpdu_t b;
    unsigned int tcn;

    make_bpdu(&b, protoSTP, bpduTypeTCN, 0, 0, 0);
    tcn = rx_tcn(p);
    port_rx_bpdu(p, &b, TCN_BPDU_SIZE - 1);
    assert_int_equal(rx_tcn(p), tcn);
    port_rx_bpdu(p, &b, TCN_BPDU_SIZE);
    assert_int_equal(rx_tcn(p), tcn + 1);
}

/* Protocol identifier other than 0 and unknown BPDU type dropped */
static void rx_unknown_protocol_or_type(void **state)
{
    port_t *p = setup(state);
    bpdu_t b;

    make_bpdu(&b, protoRSTP, bpduTypeRST, SUPERIOR_PRIO, SUPERIOR_MAC,
              DESIGNATED_FLAGS | TC_FLAG);
    b.protocolIdentifier = __cpu_to_be16(1);
    assert_dropped(p, &b, RST_BPDU_SIZE);

    make_bpdu(&b, protoRSTP, BPDU_TYPE_UNKNOWN, SUPERIOR_PRIO, SUPERIOR_MAC,
              DESIGNATED_FLAGS | TC_FLAG);
    assert_dropped(p, &b, RST_BPDU_SIZE);
}

/* RST type with version 0 or 1 dropped */
static void rx_rst_version_below_2(void **state)
{
    port_t *p = setup(state);
    bpdu_t b;

    make_bpdu(&b, protoSTP + 1, bpduTypeRST, SUPERIOR_PRIO, SUPERIOR_MAC,
              DESIGNATED_FLAGS | TC_FLAG);
    assert_dropped(p, &b, RST_BPDU_SIZE);
    b.protocolVersion = protoSTP;
    assert_dropped(p, &b, RST_BPDU_SIZE);
}

/* RST type with version > 2 (not a valid MST BPDU) handled as RST */
static void rx_version_above_2_as_rst(void **state)
{
    port_t *p = setup(state);
    CIST_PortStatus s;
    bpdu_t b;

    make_bpdu(&b, protoMSTP + 1, bpduTypeRST, SUPERIOR_PRIO, SUPERIOR_MAC,
              DESIGNATED_FLAGS | TC_FLAG);
    assert_accepted(p, &b, RST_BPDU_SIZE);

    MSTP_IN_get_cist_port_status(p, &s);
    assert_int_equal(s.role, roleRoot);
    assert_true(s.sendRSTP);
}

int main(int argc, char *argv[])
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(rx_rst_too_short, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(rx_config_too_short, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(rx_tcn_too_short, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(rx_unknown_protocol_or_type, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(rx_rst_version_below_2, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(rx_version_above_2_as_rst, prepare_test, teardown_test),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
