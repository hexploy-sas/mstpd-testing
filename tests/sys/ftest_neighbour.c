/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ftest-sys: one real mstpd facing two virtual BPDU neighbours.
 *   dut0 e1 <-> neighbour A, dut0 e2 <-> neighbour B
 * dut0 bridge ID 8.000.02:00:00:00:00:10. The neighbours send crafted
 * BPDUs and frames through AF_PACKET sockets.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <linux/if_bridge.h>
#include <linux/if_ether.h>

#include "sys.h"
#include "mstp.h"

#define START_MS 10000
#define CONVERGE_MS 15000
#define REACT_MS 3000
#define PERIOD_MS 1000
#define GARBAGE_FRAMES 200
/* time for mstpd to process the frames sent */
#define PROCESS_MS 500
/* BPDU transmit rate */
#define TX_HOLD 3
#define CHANGE_PERIOD_MS 50
#define ALT_COST 1000
#define OBSERVE_S 5
#define MAX_CAPTURED 256

#define DESIGNATED (BPDU_FLAGS_ROLE_SET(encodedRoleDesignated) \
                    | (1 << offsetLearnig) | (1 << offsetForwarding))
/* inferior neighbours send a designated role without learning/forwarding:
 * inferior designated info with the learning flag is a dispute (802.1D-2004
 * 17.21.10) and puts the port into discarding */
#define DESIGNATED_DISCARDING BPDU_FLAGS_ROLE_SET(encodedRoleDesignated)
#define TC (1 << offsetTc)

/* root and bridge IDs of the virtual neighbours */
#define A_PRIO BRIDGE_PRIO_VALUE(1)
#define A_MAC 0xa1
#define B_PRIO BRIDGE_PRIO_VALUE(0)
#define B_MAC 0xb1
#define INF_PRIO BRIDGE_PRIO_VALUE(PRIO_MAX)
#define INF_MAC 0xf1

struct topo {
    struct dut d;
    struct vnb a, b;
};

static struct topo topo;

static int setup(void **state)
{
    struct topo *t = &topo;

    if (sys_skip_reason())
        return 0;
    memset(t, 0, sizeof(*t));
    t->d.netns = t->a.fd = t->b.fd = -1;
    assert_int_equal(dut_create(&t->d, "dut0", 0x10), 0);
    assert_int_equal(vnb_link(&t->a, "nbA", 0xa, &t->d, "e1"), 0);
    assert_int_equal(vnb_link(&t->b, "nbB", 0xb, &t->d, "e2"), 0);
    assert_int_equal(dut_start(&t->d), 0);
    *state = t;
    return 0;
}

static int teardown(void **state)
{
    dut_destroy(&topo.d);
    vnb_destroy(&topo.a);
    vnb_destroy(&topo.b);
    return 0;
}

struct port_check {
    struct dut *d;
    const char *port;
    const char *role;   /* NULL: not checked */
    int state;          /* -1: not checked */
    const char *edge;   /* "yes", "no", NULL: not checked */
};

static bool port_ok(const struct port_check *c)
{
    char val[32];

    if (c->state >= 0 && dut_port_state(c->d, c->port) != c->state)
        return false;
    if (c->role && (dut_port_role(c->d, c->port, val, sizeof(val))
                    || strcmp(val, c->role)))
        return false;
    if (c->edge && (dut_port_param(c->d, c->port, "oper-edge-port", val,
                                   sizeof(val)) || strcmp(val, c->edge)))
        return false;
    return true;
}

/* NULL-terminated by a check with port == NULL */
static bool ports_ok(void *arg)
{
    const struct port_check *c;

    for (c = arg; c->port; c++)
        if (!port_ok(c))
            return false;
    return true;
}

/* print actual values, then fail */
static void report_ports(const struct port_check *c)
{
    char role[32], edge[16];

    for (; c->port; c++) {
        if (dut_port_role(c->d, c->port, role, sizeof(role)))
            strcpy(role, "?");
        if (dut_port_param(c->d, c->port, "oper-edge-port", edge, sizeof(edge)))
            strcpy(edge, "?");
        print_message("%s %s: role %s, kernel state %d, edge %s\n",
                      c->d->name, c->port, role, dut_port_state(c->d, c->port),
                      edge);
    }
}

static void assert_ports(struct port_check *c, long timeout_ms)
{
    if (sys_wait(ports_ok, c, timeout_ms) < 0) {
        report_ports(c);
        fail_msg("ports did not reach the expected roles and states");
    }
}

static bool own_root(struct dut *d)
{
    char id[64], root[64];

    assert_int_equal(dut_bridge_ids(d, id, root, sizeof(id)), 0);
    return !strcmp(id, root);
}

static long port_counter(struct dut *d, const char *port, const char *key)
{
    char val[32];

    assert_int_equal(dut_port_param(d, port, key, val, sizeof(val)), 0);
    return strtol(val, NULL, 10);
}

/* both ports up without BPDU: auto edge, designated, forwarding */
static void wait_edge_ports(struct topo *t)
{
    struct port_check c[] = {
        { &t->d, "e1", "Designated", BR_STATE_FORWARDING, "yes" },
        { &t->d, "e2", "Designated", BR_STATE_FORWARDING, "yes" },
        { NULL }
    };

    assert_ports(c, START_MS);
    assert_true(own_root(&t->d));
}

/* Superior BPDU from a virtual neighbour: the root port moves to it; a
 * better path to the same root on the other port moves it again and the
 * first port becomes alternate; kernel states follow the roles */
static void nb_superior_bpdu(void **state)
{
    struct topo *t = *state;
    unsigned char bpdu[sizeof(bpdu_t)];
    char id[64], root[64];
    struct port_check on_a[] = {
        { &t->d, "e1", "Root", BR_STATE_FORWARDING, NULL },
        { &t->d, "e2", "Designated", BR_STATE_FORWARDING, NULL },
        { NULL }
    };
    struct port_check on_b[] = {
        { &t->d, "e2", "Root", BR_STATE_FORWARDING, NULL },
        { &t->d, "e1", "Alternate", BR_STATE_BLOCKING, NULL },
        { NULL }
    };

    sys_require();
    wait_edge_ports(t);

    vnb_make_bpdu(bpdu, DESIGNATED, A_PRIO, A_MAC, 0, A_PRIO, A_MAC);
    assert_int_equal(vnb_send_periodic(&t->a, bpdu, RST_BPDU_SIZE,
                                       PERIOD_MS), 0);
    assert_ports(on_a, CONVERGE_MS);
    assert_int_equal(dut_bridge_ids(&t->d, id, root, sizeof(id)), 0);
    assert_string_equal(root, "1.000.02:00:00:00:00:A1");

    /* same root through B, whose bridge ID is better than A's */
    vnb_make_bpdu(bpdu, DESIGNATED, A_PRIO, A_MAC, 0, B_PRIO, B_MAC);
    assert_int_equal(vnb_send_periodic(&t->b, bpdu, RST_BPDU_SIZE,
                                       PERIOD_MS), 0);
    assert_ports(on_b, CONVERGE_MS);
    assert_int_equal(dut_bridge_ids(&t->d, id, root, sizeof(id)), 0);
    assert_string_equal(root, "1.000.02:00:00:00:00:A1");
}

static uint32_t xorshift32(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

/* Inferior, malformed and garbage BPDUs: ignored without crash; the root,
 * role, kernel state and TC counter do not change; all of them are
 * counted as received */
static void nb_bad_bpdus_ignored(void **state)
{
    struct topo *t = *state;
    bpdu_t b;
    unsigned char raw[80];
    uint32_t seed = 0x6d737470;
    long rx, tcn;
    int i, j, len, sent = 0;
    struct port_check c[] = {
        { &t->d, "e1", "Designated", BR_STATE_FORWARDING, NULL },
        { NULL }
    };

    sys_require();
    wait_edge_ports(t);
    rx = port_counter(&t->d, "e1", "num-rx-bpdu");
    tcn = port_counter(&t->d, "e1", "num-rx-tcn");

    /* superior with TC: would change the root if accepted */
    vnb_make_bpdu(&b, DESIGNATED | TC, A_PRIO, A_MAC, 0, A_PRIO, A_MAC);
    assert_int_equal(vnb_send(&t->a, &b, RST_BPDU_SIZE - 1), 0);
    sent++;
    b.protocolVersion = protoSTP;
    b.bpduType = bpduTypeConfig;
    assert_int_equal(vnb_send(&t->a, &b, CONFIG_BPDU_SIZE - 1), 0);
    sent++;
    b.bpduType = bpduTypeTCN;
    assert_int_equal(vnb_send(&t->a, &b, TCN_BPDU_SIZE - 1), 0);
    sent++;
    vnb_make_bpdu(&b, DESIGNATED | TC, A_PRIO, A_MAC, 0, A_PRIO, A_MAC);
    b.protocolIdentifier = htons(1);
    assert_int_equal(vnb_send(&t->a, &b, RST_BPDU_SIZE), 0);
    sent++;
    vnb_make_bpdu(&b, DESIGNATED | TC, A_PRIO, A_MAC, 0, A_PRIO, A_MAC);
    b.bpduType = 0x55;
    assert_int_equal(vnb_send(&t->a, &b, RST_BPDU_SIZE), 0);
    sent++;
    vnb_make_bpdu(&b, DESIGNATED | TC, A_PRIO, A_MAC, 0, A_PRIO, A_MAC);
    b.protocolVersion = 1;
    assert_int_equal(vnb_send(&t->a, &b, RST_BPDU_SIZE), 0);
    sent++;

    /* random payloads, protocol identifier never 0 */
    for (i = 0; i < GARBAGE_FRAMES; i++) {
        len = xorshift32(&seed) % sizeof(raw);
        for (j = 0; j < len; j++)
            raw[j] = xorshift32(&seed);
        if (len >= 2 && !raw[0] && !raw[1])
            raw[1] = 1;
        assert_int_equal(vnb_send(&t->a, raw, len), 0);
        sent++;
    }

    /* valid inferior BPDU: the port stays designated */
    vnb_make_bpdu(&b, DESIGNATED_DISCARDING, INF_PRIO, INF_MAC, 0, INF_PRIO, INF_MAC);
    assert_int_equal(vnb_send(&t->a, &b, RST_BPDU_SIZE), 0);
    sent++;

    usleep(PROCESS_MS * 1000);
    assert_true(dut_mstpd_alive(&t->d));
    assert_true(own_root(&t->d));
    assert_ports(c, 0);
    assert_int_equal(port_counter(&t->d, "e1", "num-rx-tcn"), tcn);
    assert_int_equal(port_counter(&t->d, "e1", "num-rx-bpdu"), rx + sent);
}

static bool guard_tripped(void *arg)
{
    struct topo *t = arg;
    char val[16];
    int flags = dut_link_flags(&t->d, "e1");

    return flags >= 0 && !(flags & IFF_UP)
           && !dut_port_param(&t->d, "e1", "bpdu-guard-error", val,
                              sizeof(val))
           && !strcmp(val, "yes");
}

/* BPDU guard: one BPDU on the guarded port takes the port down */
static void nb_bpdu_guard(void **state)
{
    struct topo *t = *state;
    unsigned char bpdu[sizeof(bpdu_t)];
    int flags;

    sys_require();
    wait_edge_ports(t);
    assert_int_equal(dut_ctl(&t->d, NULL, 0, "setbpduguard", "br0", "e1",
                             "yes", (char *)NULL), 0);

    vnb_make_bpdu(bpdu, DESIGNATED_DISCARDING, INF_PRIO, INF_MAC, 0, INF_PRIO, INF_MAC);
    assert_int_equal(vnb_send(&t->a, bpdu, RST_BPDU_SIZE), 0);
    assert_true(sys_wait(guard_tripped, t, REACT_MS) >= 0);

    /* the other port is not affected */
    flags = dut_link_flags(&t->d, "e2");
    assert_true(flags >= 0 && (flags & IFF_UP));
}

static const unsigned char mac_x[ETH_ALEN] = { 0x02, 0, 0, 0, 0x0b, 0x01 };
static const unsigned char mac_y[ETH_ALEN] = { 0x02, 0, 0, 0, 0x0a, 0x01 };

/* broadcast frame from src, local experimental EtherType */
static void send_from(struct vnb *n, const unsigned char *src)
{
    unsigned char f[ETH_ZLEN];
    struct ethhdr *h = (struct ethhdr *)f;

    memset(f, 0, sizeof(f));
    memset(h->h_dest, 0xff, ETH_ALEN);
    memcpy(h->h_source, src, ETH_ALEN);
    h->h_proto = htons(ETH_P_802_EX1);
    assert_int_equal(vnb_send_frame(n, f, sizeof(f)), 0);
}

struct fdb_check {
    struct dut *d;
    const char *port;
    const unsigned char *mac;
    int present;
};

static bool fdb_ok(void *arg)
{
    const struct fdb_check *c;

    for (c = arg; c->port; c++)
        if (dut_fdb_has(c->d, c->port, c->mac) != c->present)
            return false;
    return true;
}

/* TC received on the root port: the FDB entries of the other non-edge
 * port are flushed, those of the receiving port are kept */
static void nb_tc_flushes_other_ports(void **state)
{
    struct topo *t = *state;
    unsigned char bpdu[sizeof(bpdu_t)];
    struct port_check ready[] = {
        { &t->d, "e1", "Root", BR_STATE_FORWARDING, "no" },
        { &t->d, "e2", "Designated", BR_STATE_FORWARDING, "no" },
        { NULL }
    };
    struct fdb_check learnt[] = {
        { &t->d, "e1", mac_y, 1 },
        { &t->d, "e2", mac_x, 1 },
        { NULL }
    };
    struct fdb_check flushed[] = {
        { &t->d, "e2", mac_x, 0 },
        { NULL }
    };

    sys_require();
    vnb_make_bpdu(bpdu, DESIGNATED, A_PRIO, A_MAC, 0, A_PRIO, A_MAC);
    assert_int_equal(vnb_send_periodic(&t->a, bpdu, RST_BPDU_SIZE,
                                       PERIOD_MS), 0);
    vnb_make_bpdu(bpdu, DESIGNATED_DISCARDING, INF_PRIO, INF_MAC, 0, INF_PRIO, INF_MAC);
    assert_int_equal(vnb_send_periodic(&t->b, bpdu, RST_BPDU_SIZE,
                                       PERIOD_MS), 0);
    assert_ports(ready, CONVERGE_MS);

    send_from(&t->a, mac_y);
    send_from(&t->b, mac_x);
    assert_true(sys_wait(fdb_ok, learnt, REACT_MS) >= 0);

    vnb_make_bpdu(bpdu, DESIGNATED | TC, A_PRIO, A_MAC, 0, A_PRIO, A_MAC);
    assert_int_equal(vnb_send(&t->a, bpdu, RST_BPDU_SIZE), 0);
    assert_true(sys_wait(fdb_ok, flushed, REACT_MS) >= 0);
    assert_int_equal(dut_fdb_has(&t->d, "e1", mac_y), 1);
}

/* BPDU transmit rate: the root info from A changes every CHANGE_PERIOD_MS;
 * the DUT sends at most Transmit Hold Count BPDUs at once on e2, then one
 * per second (txCount decrements every second) */
static void nb_tx_rate(void **state)
{
    struct topo *t = *state;
    unsigned char b1[sizeof(bpdu_t)], b2[sizeof(bpdu_t)], rx[sizeof(bpdu_t)];
    long times[MAX_CAPTURED], start, end;
    char hold[8];
    int n = 0, i, j, max_1s = 0;
    struct port_check root[] = {
        { &t->d, "e1", "Root", BR_STATE_FORWARDING, NULL },
        { NULL }
    };

    sys_require();
    wait_edge_ports(t);
    snprintf(hold, sizeof(hold), "%d", TX_HOLD);
    assert_int_equal(dut_ctl(&t->d, NULL, 0, "settxholdcount", "br0", hold,
                             (char *)NULL), 0);

    /* each change of root path cost gives new designated info on e2 */
    vnb_make_bpdu(b1, DESIGNATED, A_PRIO, A_MAC, 0, A_PRIO, A_MAC);
    vnb_make_bpdu(b2, DESIGNATED, A_PRIO, A_MAC, ALT_COST, A_PRIO, A_MAC);
    assert_int_equal(vnb_send_alternating(&t->a, b1, b2, RST_BPDU_SIZE,
                                          CHANGE_PERIOD_MS), 0);
    assert_ports(root, CONVERGE_MS);

    while (vnb_recv(&t->b, rx, sizeof(rx), 0) >= 0)
        ;
    start = sys_now_ms();
    end = start + OBSERVE_S * 1000;
    while (sys_now_ms() < end && n < MAX_CAPTURED)
        if (vnb_recv(&t->b, rx, sizeof(rx), end - sys_now_ms()) >= 0)
            times[n++] = sys_now_ms() - start;
    vnb_stop(&t->a);

    for (i = 0; i < n; i++) {
        for (j = i; j < n && times[j] - times[i] < 1000; j++)
            ;
        if (j - i > max_1s)
            max_1s = j - i;
    }
    print_message("%d BPDUs in %d s, at most %d in 1 s\n", n, OBSERVE_S,
                  max_1s);
    assert_true(n >= OBSERVE_S);
    assert_true(n <= TX_HOLD + OBSERVE_S);
    assert_true(max_1s <= TX_HOLD + 1);
}

int main(int argc, char *argv[])
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(nb_superior_bpdu, setup, teardown),
        cmocka_unit_test_setup_teardown(nb_bad_bpdus_ignored, setup, teardown),
        cmocka_unit_test_setup_teardown(nb_bpdu_guard, setup, teardown),
        cmocka_unit_test_setup_teardown(nb_tc_flushes_other_ports, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(nb_tx_rate, setup, teardown),
    };

    sys_init();
    return cmocka_run_group_tests(tests, NULL, NULL);
}
