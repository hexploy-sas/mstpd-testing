/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * brmon.c: RTM_NEWLINK/RTM_DELLINK parsing into bridge_notify() calls.
 * brmon.c is included to reach its static listen_msg(); bridge_notify() is
 * a recorder; is_bridge() and logging come from the fake system layer.
 *   br0 (index 10), p1 (11), eth9 (13, not a bridge port)
 */

#include "brmon.c"

#include <string.h>
#include <net/if.h>

#include "sys_fake.h"

#define BR 10
#define P1 11
#define ETH 13
#define UP_RUNNING (IFF_UP | IFF_RUNNING)

struct notify_call {
    int br_index, if_index;
    bool newlink;
    unsigned int flags;
};

static struct notify_call last_call;
static int num_calls;

int bridge_notify(int br_index, int if_index, bool newlink, unsigned flags)
{
    last_call.br_index = br_index;
    last_call.if_index = if_index;
    last_call.newlink = newlink;
    last_call.flags = flags;
    num_calls++;
    return 0;
}

/* not used by listen_msg() */
int add_epoll(struct epoll_event_handler *h)
{
    return -1;
}

struct link_msg {
    struct nlmsghdr n;
    struct ifinfomsg ifi;
    char buf[256];
};

static void build(struct link_msg *m, int type, int family, int ifindex,
                  const char *name, int master)
{
    memset(m, 0, sizeof(*m));
    m->n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    m->n.nlmsg_type = type;
    m->ifi.ifi_family = family;
    m->ifi.ifi_index = ifindex;
    m->ifi.ifi_flags = UP_RUNNING;
    if (name)
        addattr_l(&m->n, sizeof(*m), IFLA_IFNAME, name, strlen(name) + 1);
    if (master >= 0)
        addattr32(&m->n, sizeof(*m), IFLA_MASTER, master);
}

static int setup(void **state)
{
    sys_fake_reset();
    sys_fake_add_bridge(BR, "br0", 0x200000000001);
    sys_fake_add_port(P1, "p1", 0x200000000011, 1);
    sys_fake_add_port(ETH, "eth9", 0x200000000013, 0);
    memset(&last_call, 0, sizeof(last_call));
    num_calls = 0;
    return 0;
}

static void assert_call(int br_index, int if_index, bool newlink)
{
    assert_int_equal(num_calls, 1);
    assert_int_equal(last_call.br_index, br_index);
    assert_int_equal(last_call.if_index, if_index);
    assert_int_equal(last_call.newlink, newlink);
    assert_int_equal(last_call.flags, UP_RUNNING);
}

/* Bridge port: AF_BRIDGE message with IFLA_MASTER -> master index */
static void brmon_port_newlink(void **state)
{
    struct link_msg m;

    build(&m, RTM_NEWLINK, AF_BRIDGE, P1, "p1", BR);
    assert_int_equal(listen_msg(NULL, &m.n, NULL), 0);
    assert_call(BR, P1, true);
}

static void brmon_port_dellink(void **state)
{
    struct link_msg m;

    build(&m, RTM_DELLINK, AF_BRIDGE, P1, "p1", BR);
    assert_int_equal(listen_msg(NULL, &m.n, NULL), 0);
    assert_call(BR, P1, false);
}

/* Bridge itself, no master: br_index = own index */
static void brmon_bridge_newlink(void **state)
{
    struct link_msg m;

    build(&m, RTM_NEWLINK, AF_UNSPEC, BR, "br0", -1);
    assert_int_equal(listen_msg(NULL, &m.n, NULL), 0);
    assert_call(BR, BR, true);
}

/* Other interface, no master: br_index -1 */
static void brmon_other_interface(void **state)
{
    struct link_msg m;

    build(&m, RTM_DELLINK, AF_UNSPEC, ETH, "eth9", -1);
    assert_int_equal(listen_msg(NULL, &m.n, NULL), 0);
    assert_call(-1, ETH, false);
}

/* Ignored: non-bridge master (bonding slave), other family, other type,
 * end of dump */
static void brmon_ignored(void **state)
{
    struct link_msg m;

    build(&m, RTM_NEWLINK, AF_UNSPEC, ETH, "eth9", 20);
    assert_int_equal(listen_msg(NULL, &m.n, NULL), 0);
    build(&m, RTM_NEWLINK, AF_INET, P1, "p1", BR);
    assert_int_equal(listen_msg(NULL, &m.n, NULL), 0);
    build(&m, RTM_NEWADDR, AF_BRIDGE, P1, "p1", BR);
    assert_int_equal(listen_msg(NULL, &m.n, NULL), 0);
    build(&m, NLMSG_DONE, AF_BRIDGE, P1, "p1", BR);
    assert_int_equal(listen_msg(NULL, &m.n, NULL), 0);
    assert_int_equal(num_calls, 0);
}

/* Malformed: no IFLA_IFNAME, message shorter than ifinfomsg */
static void brmon_malformed(void **state)
{
    struct link_msg m;

    build(&m, RTM_NEWLINK, AF_BRIDGE, P1, NULL, BR);
    assert_int_equal(listen_msg(NULL, &m.n, NULL), -1);
    build(&m, RTM_NEWLINK, AF_BRIDGE, P1, "p1", BR);
    m.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg) - 1);
    assert_int_equal(listen_msg(NULL, &m.n, NULL), -1);
    assert_int_equal(num_calls, 0);
}

int main(int argc, char *argv[])
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup(brmon_port_newlink, setup),
        cmocka_unit_test_setup(brmon_port_dellink, setup),
        cmocka_unit_test_setup(brmon_bridge_newlink, setup),
        cmocka_unit_test_setup(brmon_other_interface, setup),
        cmocka_unit_test_setup(brmon_ignored, setup),
        cmocka_unit_test_setup(brmon_malformed, setup),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
