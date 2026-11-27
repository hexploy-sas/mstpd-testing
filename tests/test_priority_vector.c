/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Port Role Selection (802.1D-2004 17.6, 17.21.25): root election by
 * bridge priority, then MAC address; root port by designated port ID.
 */

#include <stdio.h>
#include <linux/if_ether.h>
#include <linux/if_bridge.h>
#include <asm/byteorder.h>

#include "mstp.h"
#include "common.h"
#include "stp_params.h"

#define CONVERGENCE_TICKS 10
/* bridge priorities better than the default */
#define PRIO_BETTER (BRIDGE_PRIO_DEFAULT - 1)
#define PRIO_BEST (BRIDGE_PRIO_DEFAULT - 2)
/* port priority better than the default */
#define PORT_PRIO_BETTER (PORT_PRIO_DEFAULT / 2)
/* path cost well above the default of a 1 Gb/s link */
#define HIGH_COST 100000

static void set_prio(bridge_t *br, __u8 prio)
{
    assert_int_equal(MSTP_IN_set_msti_bridge_config(GET_CIST_TREE(br), prio), 0);
}

static void start(bridge_t *br0, bridge_t *br1, port_t *a, port_t *b)
{
    link_ports(a, b);
    MSTP_IN_set_bridge_enable(br0, true);
    MSTP_IN_set_bridge_enable(br1, true);
    set_port_state(a, true, SPEED_1000, DUPLEX_FULL);
}

static __u64 root_of(bridge_t *br)
{
    CIST_BridgeStatus s;

    MSTP_IN_get_cist_bridge_status(br, &s);
    return s.designated_root.u;
}

static __u64 id_of(bridge_t *br)
{
    CIST_BridgeStatus s;

    MSTP_IN_get_cist_bridge_status(br, &s);
    return s.bridge_id.u;
}

static port_role_t role_of(port_t *p)
{
    CIST_PortStatus s;

    MSTP_IN_get_cist_port_status(p, &s);
    return s.role;
}

/* Same priority: the lowest MAC address wins */
static void root_lowest_mac(void **state)
{
    bridge_t *br0, *br1;
    port_t *p0[1], *p1[1];

    assert_int_equal(alloc_bridge_ports(state, &br0, "br0", 0x200000000002,
                                        &p0, 1), 0);
    assert_int_equal(alloc_bridge_ports(state, &br1, "br1", 0x200000000001,
                                        &p1, 1), 0);
    start(br0, br1, p0[0], p1[0]);
    test_ticks(state, CONVERGENCE_TICKS);

    assert_uint_equal(root_of(br0), id_of(br1));
    assert_uint_equal(root_of(br1), id_of(br1));
    assert_int_equal(role_of(p0[0]), roleRoot);
    assert_int_equal(role_of(p1[0]), roleDesignated);
}

/* Bridge priority wins over MAC address; a priority change re-elects */
static void root_priority_over_mac(void **state)
{
    bridge_t *br0, *br1;
    port_t *p0[1], *p1[1];

    assert_int_equal(alloc_bridge_ports(state, &br0, "br0", 0x200000000002,
                                        &p0, 1), 0);
    assert_int_equal(alloc_bridge_ports(state, &br1, "br1", 0x200000000001,
                                        &p1, 1), 0);
    set_prio(br0, PRIO_BETTER);
    start(br0, br1, p0[0], p1[0]);
    test_ticks(state, CONVERGENCE_TICKS);

    assert_uint_equal(root_of(br0), id_of(br0));
    assert_uint_equal(root_of(br1), id_of(br0));
    assert_int_equal(role_of(p0[0]), roleDesignated);
    assert_int_equal(role_of(p1[0]), roleRoot);

    set_prio(br1, PRIO_BEST);
    test_ticks(state, CONVERGENCE_TICKS);

    assert_uint_equal(root_of(br0), id_of(br1));
    assert_uint_equal(root_of(br1), id_of(br1));
    assert_int_equal(role_of(p0[0]), roleRoot);
    assert_int_equal(role_of(p1[0]), roleDesignated);
}

/* Two parallel links to the root: the root port faces the lowest designated
 * port ID; a port priority change on the root moves it */
static void root_port_designated_port_id(void **state)
{
    bridge_t *br0, *br1;
    port_t *p0[2], *p1[2];
    MSTI_PortConfig cfg = {
        .port_priority = PORT_PRIO_BETTER,
        .set_port_priority = true,
    };

    assert_int_equal(alloc_bridge_ports(state, &br0, "br0", 0x200000000001,
                                        &p0, 2), 0);
    assert_int_equal(alloc_bridge_ports(state, &br1, "br1", 0x200000000002,
                                        &p1, 2), 0);
    link_ports(p0[1], p1[1]);
    start(br0, br1, p0[0], p1[0]);
    set_port_state(p0[1], true, SPEED_1000, DUPLEX_FULL);
    test_ticks(state, CONVERGENCE_TICKS);

    assert_uint_equal(root_of(br1), id_of(br0));
    assert_int_equal(role_of(p1[0]), roleRoot);
    assert_int_equal(role_of(p1[1]), roleAlternate);

    /* br0p2 gets a lower port ID than br0p1 */
    assert_int_equal(MSTP_IN_set_msti_port_config(GET_CIST_PTP_FROM_PORT(p0[1]),
                                                  &cfg), 0);
    test_ticks(state, CONVERGENCE_TICKS);

    assert_int_equal(role_of(p1[0]), roleAlternate);
    assert_int_equal(role_of(p1[1]), roleRoot);
}

/* Two parallel links: equal designated info, the lowest root path cost wins
 * over the designated port ID */
static void root_port_path_cost(void **state)
{
    bridge_t *br0, *br1;
    port_t *p0[2], *p1[2];
    CIST_PortConfig cfg = {
        .admin_external_port_path_cost = HIGH_COST,
        .set_admin_external_port_path_cost = true,
    };

    assert_int_equal(alloc_bridge_ports(state, &br0, "br0", 0x200000000001,
                                        &p0, 2), 0);
    assert_int_equal(alloc_bridge_ports(state, &br1, "br1", 0x200000000002,
                                        &p1, 2), 0);
    /* br1p1 faces the lowest designated port ID but costs more */
    assert_int_equal(MSTP_IN_set_cist_port_config(p1[0], &cfg), 0);
    link_ports(p0[1], p1[1]);
    start(br0, br1, p0[0], p1[0]);
    set_port_state(p0[1], true, SPEED_1000, DUPLEX_FULL);
    test_ticks(state, CONVERGENCE_TICKS);

    assert_int_equal(role_of(p1[0]), roleAlternate);
    assert_int_equal(role_of(p1[1]), roleRoot);
}

int main(int argc, char *argv[])
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(root_lowest_mac, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(root_priority_over_mac, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(root_port_designated_port_id, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(root_port_path_cost, prepare_test, teardown_test),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
