/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * operPointToPointMAC (802.1D-2004 6.4.3, 17.19.17): rapid transition through
 * proposal/agreement needs a point-to-point link.
 * Root br0p1 <-> br1p1, half duplex.
 */

#include <stdio.h>
#include <linux/if_ether.h>
#include <linux/if_bridge.h>
#include <asm/byteorder.h>

#include "mstp.h"
#include "common.h"
#include "stp_params.h"

#define RAPID_TICKS 3

static void build(void **state, port_t **dp, admin_p2p_t admin_p2p)
{
    bridge_t *br0, *br1;
    port_t *p0[1], *p1[1];
    CIST_PortConfig cfg = {
        .admin_p2p = admin_p2p,
        .set_admin_p2p = true,
    };

    assert_int_equal(alloc_bridge_ports(state, &br0, "br0", 0x200000000001,
                                        &p0, 1), 0);
    assert_int_equal(alloc_bridge_ports(state, &br1, "br1", 0x200000000002,
                                        &p1, 1), 0);
    assert_int_equal(MSTP_IN_set_cist_port_config(p0[0], &cfg), 0);
    assert_int_equal(MSTP_IN_set_cist_port_config(p1[0], &cfg), 0);
    link_ports(p0[0], p1[0]);
    MSTP_IN_set_bridge_enable(br0, true);
    MSTP_IN_set_bridge_enable(br1, true);
    set_port_state(p0[0], true, SPEED_100, DUPLEX_HALF);
    *dp = p0[0];
}

/* admin p2p auto + half duplex: not p2p, no rapid transition; the
 * designated port forwards only when fdWhile expires (Max Age, then Hello
 * Time of learning) */
static void p2p_auto_half_duplex(void **state)
{
    port_t *p;
    CIST_PortStatus s;

    build(state, &p, p2pAuto);
    MSTP_IN_get_cist_port_status(p, &s);
    assert_false(s.oper_p2p);

    test_ticks(state, RAPID_TICKS);
    MSTP_IN_get_cist_port_status(p, &s);
    assert_int_equal(s.role, roleDesignated);
    assert_int_not_equal(s.state, BR_STATE_FORWARDING);

    test_ticks(state, MAX_AGE_DEFAULT + HELLO_TIME - RAPID_TICKS);
    MSTP_IN_get_cist_port_status(p, &s);
    assert_int_equal(s.state, BR_STATE_FORWARDING);
}

/* admin p2p forced true on half duplex: rapid transition */
static void p2p_forced_half_duplex(void **state)
{
    port_t *p;
    CIST_PortStatus s;

    build(state, &p, p2pForceTrue);
    MSTP_IN_get_cist_port_status(p, &s);
    assert_true(s.oper_p2p);

    test_ticks(state, RAPID_TICKS);
    MSTP_IN_get_cist_port_status(p, &s);
    assert_int_equal(s.role, roleDesignated);
    assert_int_equal(s.state, BR_STATE_FORWARDING);
}

int main(int argc, char *argv[])
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(p2p_auto_half_duplex, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(p2p_forced_half_duplex, prepare_test, teardown_test),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
