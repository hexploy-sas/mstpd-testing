/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ftest-sim: RSTP functional scenarios on simulated topologies.
 * Topology: ring br0 -- br1 -- br2 -- br0, br0 has the lowest bridge ID.
 *   br0p1 <-> br1p1, br1p2 <-> br2p1, br2p2 <-> br0p2
 */

#include <stdio.h>
#include <linux/if_ether.h>
#include <linux/if_bridge.h>
#include <asm/byteorder.h>

#include "mstp.h"
#include "common.h"

#define CONVERGENCE_TICKS 10
#define RAPID_TICKS 3

struct ring {
    bridge_t *br[3];
    port_t *p[3][2];
};

static void set_rstp(bridge_t *br)
{
    CIST_BridgeConfig cfg = {
        .protocol_version = protoRSTP,
        .set_protocol_version = true,
    };

    assert_int_equal(MSTP_IN_set_cist_bridge_config(br, &cfg), 0);
}

static void tick(void **state, int n)
{
    while (n--)
        test_one_second(state);
}

static void port_status(port_t *p, CIST_PortStatus *s)
{
    MSTP_IN_get_cist_port_status(p, s);
}

static void build_ring(void **state, struct ring *r)
{
    static const __u64 macs[3] = { 0x200000000001, 0x200000000002,
                                   0x200000000003 };
    static const char *names[3] = { "br0", "br1", "br2" };
    int i;

    for (i = 0; i < 3; i++) {
        assert_int_equal(alloc_bridge_ports(state, &r->br[i], names[i],
                                            macs[i], &r->p[i], 2), 0);
        set_rstp(r->br[i]);
    }

    link_ports(r->p[0][0], r->p[1][0]);
    link_ports(r->p[1][1], r->p[2][0]);
    link_ports(r->p[2][1], r->p[0][1]);

    for (i = 0; i < 3; i++)
        MSTP_IN_set_bridge_enable(r->br[i], true);

    set_port_state(r->p[0][0], true, 1000, true);
    set_port_state(r->p[1][1], true, 1000, true);
    set_port_state(r->p[2][1], true, 1000, true);
}

static int count_role(struct ring *r, port_role_t role)
{
    CIST_PortStatus s;
    int i, j, n = 0;

    for (i = 0; i < 3; i++)
        for (j = 0; j < 2; j++) {
            port_status(r->p[i][j], &s);
            if (s.role == role)
                n++;
        }
    return n;
}

/* FT-SIM-01: root election and loop-free active topology */
static void ring_converges(void **state)
{
    struct ring r;
    CIST_BridgeStatus b0, b;
    CIST_PortStatus s;
    int i;

    build_ring(state, &r);
    tick(state, CONVERGENCE_TICKS);

    MSTP_IN_get_cist_bridge_status(r.br[0], &b0);
    for (i = 1; i < 3; i++) {
        MSTP_IN_get_cist_bridge_status(r.br[i], &b);
        assert_uint_equal(b.designated_root.u, b0.designated_root.u);
    }
    assert_uint_equal(b0.designated_root.u, b0.bridge_id.u);

    /* root bridge: both ports designated forwarding */
    for (i = 0; i < 2; i++) {
        port_status(r.p[0][i], &s);
        assert_int_equal(s.role, roleDesignated);
        assert_int_equal(s.state, BR_STATE_FORWARDING);
    }

    /* exactly one alternate (blocking) port in the ring, two root ports */
    assert_int_equal(count_role(&r, roleRoot), 2);
    assert_int_equal(count_role(&r, roleAlternate), 1);

    /* br1 < br2: the alternate port is on br2, facing br1 */
    port_status(r.p[2][0], &s);
    assert_int_equal(s.role, roleAlternate);
    assert_int_equal(s.state, BR_STATE_BLOCKING);
}

/* FT-SIM-02: root port link loss -> alternate takes over within RAPID_TICKS */
static void ring_root_link_failure(void **state)
{
    struct ring r;
    CIST_PortStatus s;

    build_ring(state, &r);
    tick(state, CONVERGENCE_TICKS);

    /* cut br0 <-> br1 */
    unlink_ports(r.p[0][0], r.p[1][0]);
    set_port_state(r.p[0][0], false, 1000, true);
    set_port_state(r.p[1][0], false, 1000, true);
    tick(state, RAPID_TICKS);

    /* br2: former alternate becomes designated towards br1 */
    port_status(r.p[2][0], &s);
    assert_int_equal(s.role, roleDesignated);
    assert_int_equal(s.state, BR_STATE_FORWARDING);

    /* br1: root port now via br2 */
    port_status(r.p[1][1], &s);
    assert_int_equal(s.role, roleRoot);
    assert_int_equal(s.state, BR_STATE_FORWARDING);

    assert_int_equal(count_role(&r, roleAlternate), 0);
}

/* FT-SIM-03: admin edge port forwards immediately, no BPDU peer */
static void edge_port_forwards(void **state)
{
    bridge_t *br;
    port_t *p[1];
    CIST_PortStatus s;
    CIST_PortConfig cfg = {
        .admin_edge_port = true,
        .set_admin_edge_port = true,
    };

    assert_int_equal(alloc_bridge_ports(state, &br, "br0", 0x200000000001,
                                        &p, 1), 0);
    set_rstp(br);
    assert_int_equal(MSTP_IN_set_cist_port_config(p[0], &cfg), 0);
    MSTP_IN_set_bridge_enable(br, true);
    set_port_state(p[0], true, 1000, true);
    tick(state, 1);

    port_status(p[0], &s);
    assert_true(s.oper_edge_port);
    assert_int_equal(s.state, BR_STATE_FORWARDING);
}

/* FT-SIM-04: BPDU guard - BPDU on guarded edge port sets the error flag */
static void bpdu_guard_trips(void **state)
{
    bridge_t *br0, *br1;
    port_t *p0[1], *p1[1];
    CIST_PortStatus s;
    CIST_PortConfig cfg = {
        .admin_edge_port = true,
        .set_admin_edge_port = true,
        .bpdu_guard_port = true,
        .set_bpdu_guard_port = true,
    };

    assert_int_equal(alloc_bridge_ports(state, &br0, "br0", 0x200000000001,
                                        &p0, 1), 0);
    assert_int_equal(alloc_bridge_ports(state, &br1, "br1", 0x200000000002,
                                        &p1, 1), 0);
    set_rstp(br0);
    set_rstp(br1);
    assert_int_equal(MSTP_IN_set_cist_port_config(p0[0], &cfg), 0);
    link_ports(p0[0], p1[0]);
    MSTP_IN_set_bridge_enable(br0, true);
    MSTP_IN_set_bridge_enable(br1, true);
    set_port_state(p0[0], true, 1000, true);
    tick(state, 3);

    port_status(p0[0], &s);
    assert_true(s.bpdu_guard_error);
    /* Port shutdown is driver-side (MSTP_OUT_shutdown_port), a no-op in
     * tests/common.c: link-down effect is covered by ftest-sys. */
}

int main(int argc, char *argv[])
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(ring_converges, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(ring_root_link_failure, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(edge_port_forwards, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(bpdu_guard_trips, prepare_test, teardown_test),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
