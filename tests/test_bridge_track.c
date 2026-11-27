/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * bridge_track.c: interface tracking from netlink notifications, mstpctl
 * addbridge/delbridge, kernel STP state updates and BPDU reception.
 * Real bridge_track.c and mstp.c on top of the fake system layer.
 *   br0 (index 10) with ports p1 (11) and p2 (12)
 */

#include <stdio.h>
#include <string.h>
#include <net/if.h>
#include <linux/if_bridge.h>
#include <asm/byteorder.h>

#include "sys_fake.h"
#include "bridge_ctl.h"
#include "ctl_functions.h"
#include "mstp.h"
#include "stp_params.h"
#include "libnetlink.h"

#define BR 10
#define P1 11
#define P2 12
#define UP_RUNNING (IFF_UP | IFF_RUNNING)
#define TICKS 6
/* LLC header of a BPDU frame: DSAP, SSAP, control */
#define LLC_HLEN 3
/* receive buffer of packet.c */
#define RX_BUF_LEN 2048

/* defined by brmon.c in mstpd */
struct rtnl_handle rth_state;

static int setup(void **state)
{
    sys_fake_reset();
    sys_fake_add_bridge(BR, "br0", 0x200000000001);
    sys_fake_add_port(P1, "p1", 0x200000000011, 1);
    sys_fake_add_port(P2, "p2", 0x200000000012, 2);
    sys_fake_set_flags(BR, UP_RUNNING);
    return 0;
}

static int teardown(void **state)
{
    /* DELLINK of the bridge itself frees all mstpd data */
    bridge_notify(-1, BR, false, 0);
    sys_fake_reset();
    return 0;
}

static void ticks(int n)
{
    while (n--)
        bridge_one_second();
}

/* netlink notification for a bridge port */
static void notify_port(int ifindex, bool newlink, unsigned int flags)
{
    sys_fake_set_flags(ifindex, flags);
    assert_int_equal(bridge_notify(BR, ifindex, newlink, flags), 0);
}

static void notify_bridge(void)
{
    assert_int_equal(bridge_notify(BR, BR, true, UP_RUNNING), 0);
}

static void addbridge(void)
{
    int br_array[] = { 1, BR };

    assert_int_equal(CTL_add_bridges(br_array), 0);
}

static void delbridge(void)
{
    int br_array[] = { 1, BR };

    assert_int_equal(CTL_del_bridges(br_array), 0);
}

static int port_status(int ifindex, CIST_PortStatus *s)
{
    return CTL_get_cist_port_status(BR, ifindex, s);
}

/* Ports added or removed before mstpctl addbridge are tracked (3ccdf6b):
 * no STP before addbridge; then exactly the current ports are managed */
static void bt_ports_before_addbridge(void **state)
{
    CIST_PortStatus s;
    int last;

    notify_bridge();
    notify_port(P1, true, UP_RUNNING);
    notify_port(P2, true, UP_RUNNING);
    notify_port(P2, false, UP_RUNNING);

    ticks(TICKS);
    assert_int_equal(sys_fake_num_tx(P1), 0);
    assert_int_equal(sys_fake_num_set_state(P1, NULL), 0);

    addbridge();
    assert_int_equal(port_status(P1, &s), 0);
    assert_true(s.enabled);

    ticks(TICKS);
    assert_true(sys_fake_num_tx(P1) > 0);
    sys_fake_num_set_state(P1, &last);
    assert_int_equal(last, BR_STATE_FORWARDING);
    assert_int_equal(sys_fake_num_errors(), 0);

    /* unknown port: logs an error */
    assert_int_equal(port_status(P2, &s), -1);
}

/* A port notified before its bridge is known auto-creates the bridge in
 * monitoring mode */
static void bt_port_before_bridge(void **state)
{
    CIST_PortStatus s;

    notify_port(P1, true, UP_RUNNING);
    addbridge();
    assert_int_equal(port_status(P1, &s), 0);
    assert_true(s.enabled);
}

/* Port added and removed after addbridge: enabled at once, then gone */
static void bt_port_after_addbridge(void **state)
{
    CIST_PortStatus s;
    int last;

    notify_bridge();
    addbridge();
    notify_port(P1, true, UP_RUNNING);
    assert_int_equal(port_status(P1, &s), 0);
    assert_true(s.enabled);

    ticks(TICKS);
    sys_fake_num_set_state(P1, &last);
    assert_int_equal(last, BR_STATE_FORWARDING);

    notify_port(P1, false, UP_RUNNING);
    assert_int_equal(sys_fake_num_errors(), 0);
    assert_int_equal(port_status(P1, &s), -1);
}

/* delbridge: ports disabled, no more BPDUs */
static void bt_delbridge(void **state)
{
    CIST_PortStatus s;
    int tx;

    notify_bridge();
    notify_port(P1, true, UP_RUNNING);
    addbridge();
    ticks(TICKS);

    delbridge();
    assert_int_equal(port_status(P1, &s), 0);
    assert_false(s.enabled);
    tx = sys_fake_num_tx(P1);
    ticks(TICKS);
    assert_int_equal(sys_fake_num_tx(P1), tx);
}

/* No kernel STP state change on a port without carrier (d3f5387), and no
 * error; states are set once the carrier is up */
static void bt_down_port_no_kernel_state(void **state)
{
    int n, last;

    notify_bridge();
    notify_port(P1, true, UP_RUNNING);
    notify_port(P2, true, IFF_UP);
    addbridge();
    ticks(TICKS);

    assert_true(sys_fake_num_set_state(P1, NULL) > 0);
    assert_int_equal(sys_fake_num_set_state(P2, NULL), 0);
    assert_int_equal(sys_fake_num_errors(), 0);

    notify_port(P2, true, UP_RUNNING);
    ticks(TICKS);
    assert_true(sys_fake_num_set_state(P2, &last) > 0);
    assert_int_equal(last, BR_STATE_FORWARDING);

    notify_port(P2, true, IFF_UP);
    n = sys_fake_num_set_state(P2, NULL);
    ticks(TICKS);
    assert_int_equal(sys_fake_num_set_state(P2, NULL), n);
    assert_int_equal(sys_fake_num_errors(), 0);
}

/* 802.3 frame with LLC header and an RST BPDU */
static size_t build_frame(unsigned char *buf)
{
    static const unsigned char head[] = {
        0x01, 0x80, 0xc2, 0x00, 0x00, 0x00,     /* bridge group address */
        0x02, 0x00, 0x00, 0x00, 0x00, 0x99,     /* source */
        0x00, RST_BPDU_SIZE + LLC_HLEN,         /* 802.3 length */
        0x42, 0x42, 0x03,                       /* LLC */
    };
    bpdu_t b;

    memset(&b, 0, sizeof(b));
    b.protocolVersion = protoRSTP;
    b.bpduType = bpduTypeRST;
    b.flags = BPDU_FLAGS_ROLE_SET(encodedRoleDesignated);
    b.cistRootID.s.priority = __cpu_to_be16(BRIDGE_PRIO_VALUE(PRIO_MAX));
    b.cistRRootID = b.cistRootID;
    b.cistPortID = __cpu_to_be16(PORT_ID(PORT_PRIO_DEFAULT, 1));
    b.MaxAge[0] = MAX_AGE_DEFAULT;
    b.HelloTime[0] = HELLO_TIME;
    b.ForwardDelay[0] = FWD_DELAY_DEFAULT;

    memcpy(buf, head, sizeof(head));
    memcpy(buf + sizeof(head), &b, RST_BPDU_SIZE);
    return sizeof(head) + RST_BPDU_SIZE;
}

/* BPDU on a port without carrier ignored without error (e353c42); BPDU on
 * an up port passed to mstp.c */
static void bt_bpdu_on_down_port(void **state)
{
    /* same size as the receive buffer of packet.c: mstp.c copies a whole
     * bpdu_t from the frame, whatever its length */
    unsigned char frame[RX_BUF_LEN] = { 0 };
    size_t len = build_frame(frame);
    CIST_PortStatus s;

    notify_bridge();
    notify_port(P1, true, UP_RUNNING);
    notify_port(P2, true, IFF_UP);
    addbridge();
    ticks(1);

    bridge_bpdu_rcv(P2, frame, len);
    assert_int_equal(port_status(P2, &s), 0);
    assert_int_equal(s.num_rx_bpdu, 0);
    assert_int_equal(sys_fake_num_errors(), 0);

    bridge_bpdu_rcv(P1, frame, len);
    assert_int_equal(port_status(P1, &s), 0);
    assert_int_equal(s.num_rx_bpdu, 1);
}

int main(int argc, char *argv[])
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(bt_ports_before_addbridge, setup, teardown),
        cmocka_unit_test_setup_teardown(bt_port_before_bridge, setup, teardown),
        cmocka_unit_test_setup_teardown(bt_port_after_addbridge, setup, teardown),
        cmocka_unit_test_setup_teardown(bt_delbridge, setup, teardown),
        cmocka_unit_test_setup_teardown(bt_down_port_no_kernel_state, setup, teardown),
        cmocka_unit_test_setup_teardown(bt_bpdu_on_down_port, setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
