/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ftest-sim: RSTP functional scenarios on simulated topologies.
 * Topology: ring br0 -- br1 -- br2 -- br0, br0 has the lowest bridge ID.
 *   link 0: br0p1 <-> br1p1
 *   link 1: br1p2 <-> br2p1
 *   link 2: br2p2 <-> br0p2
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
/* info received from a silent neighbour ages out after 3 hello times */
#define AGE_OUT_TICKS RCVD_INFO_TIME
/* longest legacy path: 2 x default forward delay */
#define SETTLE_TICKS 30

#define NUM_BR 3
#define NUM_LINKS 3

struct ring {
    bridge_t *br[NUM_BR];
    port_t *p[NUM_BR][2];
};

struct snapshot {
    port_role_t role[NUM_BR][2];
    int state[NUM_BR][2];
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

static void link_ends(struct ring *r, int link, port_t **a, port_t **b)
{
    switch (link) {
    case 0:
        *a = r->p[0][0];
        *b = r->p[1][0];
        break;
    case 1:
        *a = r->p[1][1];
        *b = r->p[2][0];
        break;
    default:
        *a = r->p[2][1];
        *b = r->p[0][1];
        break;
    }
}

/* create and link the ring; links listed in 'up' are brought up */
static void build_ring_links(void **state, struct ring *r, const bool up[NUM_LINKS])
{
    static const __u64 macs[NUM_BR] = { 0x200000000001, 0x200000000002,
                                        0x200000000003 };
    static const char *names[NUM_BR] = { "br0", "br1", "br2" };
    port_t *a, *b;
    int i;

    for (i = 0; i < NUM_BR; i++) {
        assert_int_equal(alloc_bridge_ports(state, &r->br[i], names[i],
                                            macs[i], &r->p[i], 2), 0);
        set_rstp(r->br[i]);
    }

    for (i = 0; i < NUM_LINKS; i++) {
        link_ends(r, i, &a, &b);
        link_ports(a, b);
    }

    for (i = 0; i < NUM_BR; i++)
        MSTP_IN_set_bridge_enable(r->br[i], true);

    for (i = 0; i < NUM_LINKS; i++) {
        if (!up[i])
            continue;
        link_ends(r, i, &a, &b);
        set_port_state(a, true, SPEED_1000, DUPLEX_FULL);
    }
}

static void build_ring(void **state, struct ring *r)
{
    static const bool all_up[NUM_LINKS] = { true, true, true };

    build_ring_links(state, r, all_up);
}

static int count_role(struct ring *r, port_role_t role)
{
    CIST_PortStatus s;
    int i, j, n = 0;

    for (i = 0; i < NUM_BR; i++)
        for (j = 0; j < 2; j++) {
            port_status(r->p[i][j], &s);
            if (s.role == role)
                n++;
        }
    return n;
}

static bool port_forwarding(port_t *p)
{
    CIST_PortStatus s;

    port_status(p, &s);
    return s.state == BR_STATE_FORWARDING;
}

/* a loop exists when every link of the ring forwards at both ends */
static bool ring_has_loop(struct ring *r)
{
    port_t *a, *b;
    int i;

    for (i = 0; i < NUM_LINKS; i++) {
        link_ends(r, i, &a, &b);
        if (!a->sysdeps.up || !b->sysdeps.up)
            return false;
        if (!port_forwarding(a) || !port_forwarding(b))
            return false;
    }
    return true;
}

static void tick_no_loop(void **state, struct ring *r, int n)
{
    while (n--) {
        test_one_second(state);
        assert_false(ring_has_loop(r));
    }
}

static void take_snapshot(struct ring *r, struct snapshot *snap)
{
    CIST_PortStatus s;
    int i, j;

    for (i = 0; i < NUM_BR; i++)
        for (j = 0; j < 2; j++) {
            port_status(r->p[i][j], &s);
            snap->role[i][j] = s.role;
            snap->state[i][j] = s.state;
        }
}

static void assert_snapshot_equal(const struct snapshot *a,
                                  const struct snapshot *b)
{
    int i, j;

    for (i = 0; i < NUM_BR; i++)
        for (j = 0; j < 2; j++) {
            assert_int_equal(a->role[i][j], b->role[i][j]);
            assert_int_equal(a->state[i][j], b->state[i][j]);
        }
}

/* br1 and br2 agree that br1 is root, and forward on their shared link */
static void assert_br1_is_root(struct ring *r)
{
    CIST_BridgeStatus b1, b2;
    CIST_PortStatus s;

    MSTP_IN_get_cist_bridge_status(r->br[1], &b1);
    MSTP_IN_get_cist_bridge_status(r->br[2], &b2);
    assert_uint_equal(b1.designated_root.u, b1.bridge_id.u);
    assert_uint_equal(b2.designated_root.u, b1.bridge_id.u);

    port_status(r->p[1][1], &s);
    assert_int_equal(s.role, roleDesignated);
    assert_int_equal(s.state, BR_STATE_FORWARDING);

    port_status(r->p[2][0], &s);
    assert_int_equal(s.role, roleRoot);
    assert_int_equal(s.state, BR_STATE_FORWARDING);
}

/* port up as seen by this end only: the peer has not detected the link yet */
static void port_up_local(port_t *p)
{
    p->sysdeps.up = true;
    p->sysdeps.speed = SPEED_1000;
    p->sysdeps.duplex = true;
    MSTP_IN_set_port_enable(p, true, SPEED_1000, DUPLEX_FULL);
}

/* Ring of 3, lowest MAC root */
static void ring_converges(void **state)
{
    struct ring r;
    CIST_BridgeStatus b0, b;
    CIST_PortStatus s;
    int i;

    build_ring(state, &r);
    tick(state, CONVERGENCE_TICKS);

    MSTP_IN_get_cist_bridge_status(r.br[0], &b0);
    for (i = 1; i < NUM_BR; i++) {
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

/* Ring of 3, root-port link cut: alternate takes over within RAPID_TICKS */
static void ring_root_link_failure(void **state)
{
    struct ring r;
    CIST_PortStatus s;

    build_ring(state, &r);
    tick(state, CONVERGENCE_TICKS);

    /* cut br0 <-> br1 */
    unlink_ports(r.p[0][0], r.p[1][0]);
    set_port_state(r.p[0][0], false, SPEED_1000, DUPLEX_FULL);
    set_port_state(r.p[1][0], false, SPEED_1000, DUPLEX_FULL);
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

/* Ring of 3, root bridge removed with link loss: br1 is root within
 * RAPID_TICKS */
static void ring_root_bridge_removed(void **state)
{
    struct ring r;

    build_ring(state, &r);
    tick(state, CONVERGENCE_TICKS);

    /* br0 powers off: both its links go down */
    set_port_state(r.p[0][0], false, SPEED_1000, DUPLEX_FULL);
    set_port_state(r.p[0][1], false, SPEED_1000, DUPLEX_FULL);
    tick(state, RAPID_TICKS);

    assert_br1_is_root(&r);
}

/* Ring of 3, root bridge silent (links stay up, no more BPDUs): the stale
 * root info is kept until it ages out, then br1 is root within RAPID_TICKS */
static void ring_root_bridge_silent(void **state)
{
    struct ring r;
    CIST_BridgeStatus b0, b2;

    build_ring(state, &r);
    tick(state, CONVERGENCE_TICKS);
    MSTP_IN_get_cist_bridge_status(r.br[0], &b0);

    /* br0 stops sending BPDUs; carriers stay up */
    unlink_ports(r.p[0][0], r.p[1][0]);
    unlink_ports(r.p[2][1], r.p[0][1]);

    tick(state, AGE_OUT_TICKS - 2);
    MSTP_IN_get_cist_bridge_status(r.br[2], &b2);
    assert_uint_equal(b2.designated_root.u, b0.bridge_id.u);

    tick(state, 2 + RAPID_TICKS);
    assert_br1_is_root(&r);
}

/* Auto-edge transient loop: the last ring link comes up, one end detects
 * the carrier up to 4 ticks after the other. No loop at any tick, and the
 * ring converges to the same topology as a normal start. */
static void ring_auto_edge_no_transient_loop(void **state)
{
    static const bool link1_down[NUM_LINKS] = { true, false, true };
    struct snapshot ref, snap;
    struct ring r;
    port_t *first, *second;
    int delay, order;

    build_ring(state, &r);
    tick(state, SETTLE_TICKS);
    take_snapshot(&r, &ref);

    for (order = 0; order < 2; order++)
        for (delay = 0; delay <= 4; delay++) {
            teardown_test(state);
            assert_int_equal(prepare_test(state), 0);

            build_ring_links(state, &r, link1_down);
            tick_no_loop(state, &r, CONVERGENCE_TICKS);

            first = order ? r.p[2][0] : r.p[1][1];
            second = order ? r.p[1][1] : r.p[2][0];

            port_up_local(first);
            assert_false(ring_has_loop(&r));
            tick_no_loop(state, &r, delay);
            port_up_local(second);
            assert_false(ring_has_loop(&r));
            tick_no_loop(state, &r, SETTLE_TICKS);

            take_snapshot(&r, &snap);
            assert_snapshot_equal(&ref, &snap);
        }
}

/* xorshift32: deterministic pseudo-random sequence */
static __u32 next_rand(__u32 *seed)
{
    __u32 x = *seed;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *seed = x;
}

/* Ring, 1000 seeded random link flaps: no loop at any tick, final topology
 * equals the initial one */
static void ring_random_link_flaps(void **state)
{
    struct snapshot ref, snap;
    struct ring r;
    __u32 seed = 0x6d737470;
    port_t *a, *b;
    int i, link;

    print_message("seed 0x%08x\n", seed);

    build_ring(state, &r);
    tick(state, SETTLE_TICKS);
    take_snapshot(&r, &ref);

    for (i = 0; i < 1000; i++) {
        link = next_rand(&seed) % NUM_LINKS;
        link_ends(&r, link, &a, &b);
        set_port_state(a, !a->sysdeps.up, SPEED_1000, DUPLEX_FULL);
        assert_false(ring_has_loop(&r));
        tick_no_loop(state, &r, next_rand(&seed) % 4);
    }

    for (link = 0; link < NUM_LINKS; link++) {
        link_ends(&r, link, &a, &b);
        set_port_state(a, true, SPEED_1000, DUPLEX_FULL);
    }
    tick_no_loop(state, &r, SETTLE_TICKS);

    take_snapshot(&r, &snap);
    assert_snapshot_equal(&ref, &snap);
}

/* Admin edge port forwards immediately, no BPDU peer */
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
    set_port_state(p[0], true, SPEED_1000, DUPLEX_FULL);
    tick(state, 1);

    port_status(p[0], &s);
    assert_true(s.oper_edge_port);
    assert_int_equal(s.state, BR_STATE_FORWARDING);
}

/* BPDU guard: BPDU on guarded edge port sets the error flag */
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
    set_port_state(p0[0], true, SPEED_1000, DUPLEX_FULL);
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
        cmocka_unit_test_setup_teardown(ring_root_bridge_removed, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(ring_root_bridge_silent, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(ring_auto_edge_no_transient_loop, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(ring_random_link_flaps, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(edge_port_forwards, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(bpdu_guard_trips, prepare_test, teardown_test),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
