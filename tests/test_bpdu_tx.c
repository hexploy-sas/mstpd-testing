/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Transmitted RST BPDUs (802.1D-2004 9.3.3, 17.21.19 txRstp): flags, priority
 * vector and times match the port state.
 * Root br0 (Max Age 10, Forward Delay 7), br0p1 <-> br1p1.
 */

#include <stdio.h>
#include <linux/if_ether.h>
#include <linux/if_bridge.h>
#include <asm/byteorder.h>

#include "mstp.h"
#include "common.h"
#include "stp_params.h"

/* non-default times, to check that they propagate */
#define MAX_AGE 10
#define FWD_DELAY 7
#define CONVERGENCE_TICKS 10
#define TC_CLEAR_TICKS (2 * MAX_AGE)

#define FLAG(f) (1 << (f))

struct topo {
    bridge_t *br0, *br1, *br2;
    port_t *p0[2], *p1[1], *p2[1];
};

static void build(void **state, struct topo *t)
{
    CIST_BridgeConfig cfg = {
        .bridge_max_age = MAX_AGE,
        .set_bridge_max_age = true,
        .bridge_forward_delay = FWD_DELAY,
        .set_bridge_forward_delay = true,
    };

    assert_int_equal(alloc_bridge_ports(state, &t->br0, "br0", 0x200000000001,
                                        &t->p0, 2), 0);
    assert_int_equal(alloc_bridge_ports(state, &t->br1, "br1", 0x200000000002,
                                        &t->p1, 1), 0);
    assert_int_equal(alloc_bridge_ports(state, &t->br2, "br2", 0x200000000003,
                                        &t->p2, 1), 0);
    assert_int_equal(MSTP_IN_set_cist_bridge_config(t->br0, &cfg), 0);
    link_ports(t->p0[0], t->p1[0]);
    link_ports(t->p0[1], t->p2[0]);
    MSTP_IN_set_bridge_enable(t->br0, true);
    MSTP_IN_set_bridge_enable(t->br1, true);
    MSTP_IN_set_bridge_enable(t->br2, true);
}

static bpdu_t *last_tx(port_t *p)
{
    bpdu_t *b;
    size_t len;

    assert_int_equal(port_last_tx_bpdu(p, &b, &len), 0);
    assert_int_equal(len, RST_BPDU_SIZE);
    assert_int_equal(b->protocolVersion, protoRSTP);
    assert_int_equal(b->bpduType, bpduTypeRST);

    return b;
}

static void assert_vector(bpdu_t *b, bridge_t *root, __u32 cost,
                          bridge_t *designated, port_t *port, __u8 msg_age)
{
    CIST_BridgeStatus rs, ds;
    CIST_PortStatus ps;

    MSTP_IN_get_cist_bridge_status(root, &rs);
    MSTP_IN_get_cist_bridge_status(designated, &ds);
    MSTP_IN_get_cist_port_status(port, &ps);

    assert_uint_equal(b->cistRootID.u, rs.bridge_id.u);
    assert_int_equal(__be32_to_cpu(b->cistExtRootPathCost), cost);
    assert_uint_equal(b->cistRRootID.u, ds.bridge_id.u);
    assert_int_equal(b->cistPortID, ps.port_id);
    assert_int_equal(b->MessageAge[0], msg_age);
    assert_int_equal(b->MaxAge[0], MAX_AGE);
    assert_int_equal(b->HelloTime[0], HELLO_TIME);
    assert_int_equal(b->ForwardDelay[0], FWD_DELAY);
}

/* Designated port just enabled, no agreement yet: proposal, discarding */
static void tx_designated_proposal(void **state)
{
    struct topo t;
    bpdu_t *b;

    build(state, &t);
    unlink_ports(t.p0[0], t.p1[0]);
    set_port_state(t.p0[0], true, SPEED_1000, DUPLEX_FULL);

    b = last_tx(t.p0[0]);
    assert_int_equal(BPDU_FLAGS_ROLE_GET(b->flags), encodedRoleDesignated);
    assert_true(b->flags & FLAG(offsetProposal));
    assert_false(b->flags & FLAG(offsetLearnig));
    assert_false(b->flags & FLAG(offsetForwarding));
    assert_vector(b, t.br0, 0, t.br0, t.p0[0], 0);
}

/* Root designated port after agreement: learning, forwarding, no proposal */
static void tx_designated_forwarding(void **state)
{
    struct topo t;
    bpdu_t *b;

    build(state, &t);
    set_port_state(t.p0[0], true, SPEED_1000, DUPLEX_FULL);
    test_ticks(state, CONVERGENCE_TICKS);

    port_reset_last_tx_bpdu(t.p0[0]);
    test_ticks(state, HELLO_TIME);
    b = last_tx(t.p0[0]);
    assert_int_equal(BPDU_FLAGS_ROLE_GET(b->flags), encodedRoleDesignated);
    assert_false(b->flags & FLAG(offsetProposal));
    assert_true(b->flags & FLAG(offsetLearnig));
    assert_true(b->flags & FLAG(offsetForwarding));
    assert_vector(b, t.br0, 0, t.br0, t.p0[0], 0);
}

/* Root port: agreement, forwarding, root path cost and Message Age + 1 */
static void tx_root_port_agreement(void **state)
{
    struct topo t;
    CIST_PortStatus ps;
    bpdu_t *b;

    build(state, &t);
    set_port_state(t.p0[0], true, SPEED_1000, DUPLEX_FULL);
    test_ticks(state, CONVERGENCE_TICKS);

    MSTP_IN_get_cist_port_status(t.p1[0], &ps);
    assert_int_equal(ps.role, roleRoot);

    b = last_tx(t.p1[0]);
    assert_int_equal(BPDU_FLAGS_ROLE_GET(b->flags), encodedRoleRoot);
    assert_true(b->flags & FLAG(offsetAgreement));
    assert_false(b->flags & FLAG(offsetProposal));
    assert_true(b->flags & FLAG(offsetLearnig));
    assert_true(b->flags & FLAG(offsetForwarding));
    assert_vector(b, t.br0, ps.external_port_path_cost, t.br1, t.p1[0], 1);
}

/* TC flag set while tcWhile runs after a topology change, then cleared */
static void tx_tc_flag(void **state)
{
    struct topo t;
    bpdu_t *b;
    int i, tc = 0;

    build(state, &t);
    set_port_state(t.p0[0], true, SPEED_1000, DUPLEX_FULL);
    test_ticks(state, TC_CLEAR_TICKS);

    port_reset_last_tx_bpdu(t.p0[0]);
    test_ticks(state, HELLO_TIME);
    b = last_tx(t.p0[0]);
    assert_false(b->flags & FLAG(offsetTc));

    /* br0p2 becomes forwarding: topology change propagated to br0p1 */
    set_port_state(t.p0[1], true, SPEED_1000, DUPLEX_FULL);
    for (i = 0; i < HELLO_TIME + 1; i++) {
        test_ticks(state, 1);
        b = last_tx(t.p0[0]);
        if (b->flags & FLAG(offsetTc))
            tc++;
    }
    assert_true(tc > 0);

    test_ticks(state, TC_CLEAR_TICKS);
    port_reset_last_tx_bpdu(t.p0[0]);
    test_ticks(state, HELLO_TIME);
    b = last_tx(t.p0[0]);
    assert_false(b->flags & FLAG(offsetTc));
}

int main(int argc, char *argv[])
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(tx_designated_proposal, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(tx_designated_forwarding, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(tx_root_port_agreement, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(tx_tc_flag, prepare_test, teardown_test),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
