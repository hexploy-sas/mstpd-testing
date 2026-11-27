/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <net/if.h>
#include <linux/if_ether.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>

#include "sys_fake.h"
#include "libnetlink.h"
#include "netif_utils.h"
#include "packet.h"
#include "log.h"

#define MAX_FAKE_IFS 16
#define LOG_STRING_LEN 256

struct fake_if {
    int ifindex;
    char name[IFNAMSIZ];
    unsigned char mac[ETH_ALEN];
    bool bridge;
    int portno;
    unsigned int flags;
    int num_set_state;
    int last_state;
    int num_tx;
    int num_shutdown;
    int num_flush;
    int num_set_ageing;
};

static struct fake_if ifs[MAX_FAKE_IFS];
static int num_ifs;
static int num_errors;

int log_level = 0;
int ctl_in_handler = 0;

static struct fake_if *by_index(int ifindex)
{
    int i;

    for (i = 0; i < num_ifs; i++)
        if (ifs[i].ifindex == ifindex)
            return &ifs[i];
    return NULL;
}

static struct fake_if *by_name(const char *name)
{
    int i;

    for (i = 0; i < num_ifs; i++)
        if (!strcmp(ifs[i].name, name))
            return &ifs[i];
    return NULL;
}

static struct fake_if *add_if(int ifindex, const char *name, __u64 mac)
{
    struct fake_if *f;
    int i;

    assert_true(num_ifs < MAX_FAKE_IFS);
    f = &ifs[num_ifs++];
    memset(f, 0, sizeof(*f));
    f->ifindex = ifindex;
    strncpy(f->name, name, IFNAMSIZ - 1);
    for (i = 5; i >= 0; i--) {
        f->mac[i] = mac & 0xffu;
        mac >>= 8;
    }
    f->last_state = -1;

    return f;
}

void sys_fake_reset(void)
{
    memset(ifs, 0, sizeof(ifs));
    num_ifs = 0;
    num_errors = 0;
}

void sys_fake_add_bridge(int ifindex, const char *name, __u64 mac)
{
    add_if(ifindex, name, mac)->bridge = true;
}

void sys_fake_add_port(int ifindex, const char *name, __u64 mac, int portno)
{
    add_if(ifindex, name, mac)->portno = portno;
}

void sys_fake_set_flags(int ifindex, unsigned int flags)
{
    struct fake_if *f = by_index(ifindex);

    assert_non_null(f);
    f->flags = flags;
}

int sys_fake_num_set_state(int ifindex, int *last_state)
{
    struct fake_if *f = by_index(ifindex);

    assert_non_null(f);
    if (last_state)
        *last_state = f->last_state;
    return f->num_set_state;
}

int sys_fake_num_tx(int ifindex)
{
    struct fake_if *f = by_index(ifindex);

    assert_non_null(f);
    return f->num_tx;
}

int sys_fake_num_shutdown(int ifindex)
{
    struct fake_if *f = by_index(ifindex);

    assert_non_null(f);
    return f->num_shutdown;
}

int sys_fake_num_flush(int ifindex)
{
    struct fake_if *f = by_index(ifindex);

    assert_non_null(f);
    return f->num_flush;
}

int sys_fake_num_set_ageing(int ifindex)
{
    struct fake_if *f = by_index(ifindex);

    assert_non_null(f);
    return f->num_set_ageing;
}

int sys_fake_num_errors(void)
{
    return num_errors;
}

/* netif_utils.h */

char *index_to_name(int index, char *name)
{
    struct fake_if *f = by_index(index);

    if (!f)
        return NULL;
    strcpy(name, f->name);
    return name;
}

char *index_to_port_name(int index, char *name)
{
    return index_to_name(index, name);
}

int get_hwaddr(char *ifname, unsigned char *hwaddr)
{
    struct fake_if *f = by_name(ifname);

    if (!f)
        return -1;
    memcpy(hwaddr, f->mac, ETH_ALEN);
    return 0;
}

int get_flags(char *ifname)
{
    struct fake_if *f = by_name(ifname);

    return f ? (int)f->flags : -1;
}

int if_shutdown(char *ifname)
{
    struct fake_if *f = by_name(ifname);

    if (!f)
        return -1;
    f->num_shutdown++;
    f->flags &= ~(IFF_UP | IFF_RUNNING);
    return 0;
}

int ethtool_get_speed_duplex(char *ifname, int *speed, int *duplex)
{
    *speed = 1000;
    *duplex = 1;
    return 0;
}

bool is_bridge(char *if_name)
{
    struct fake_if *f = by_name(if_name);

    return f && f->bridge;
}

int get_bridge_portno(char *if_name)
{
    struct fake_if *f = by_name(if_name);

    return f ? f->portno : -1;
}

/* packet.h */

void packet_send(int ifindex, const struct iovec *iov, int iov_count, int len)
{
    struct fake_if *f = by_index(ifindex);

    assert_non_null(f);
    f->num_tx++;
}

/* libnetlink.h: bridge_track.c sends RTM_SETLINK + IFLA_PROTINFO only */

int __wrap_rtnl_talk(struct rtnl_handle *rtnl, struct nlmsghdr *n,
                     struct nlmsghdr **answer)
{
    struct ifinfomsg *ifi = NLMSG_DATA(n);
    struct rtattr *tb[IFLA_MAX + 1];
    struct fake_if *f;

    assert_int_equal(n->nlmsg_type, RTM_SETLINK);
    f = by_index(ifi->ifi_index);
    assert_non_null(f);

    parse_rtattr(tb, IFLA_MAX, IFLA_RTA(ifi),
                 n->nlmsg_len - NLMSG_LENGTH(sizeof(*ifi)));
    assert_non_null(tb[IFLA_PROTINFO]);

    /* the kernel refuses STP state changes on ports without carrier */
    if (!(f->flags & IFF_RUNNING))
        return -1;

    f->num_set_state++;
    f->last_state = *(__u8 *)RTA_DATA(tb[IFLA_PROTINFO]);
    return 0;
}

/* sysfs writes of bridge_track.c (linked with -Wl,--wrap=open):
 * <ifname>/brport/flush and <brname>/bridge/ageing_time are counted and
 * redirected to /dev/null; other paths are opened for real */

#define SYSFS_NET "/sys/class/net/"

int __real_open(const char *path, int flags, ...);

int __wrap_open(const char *path, int flags, ...)
{
    char name[IFNAMSIZ];
    const char *rest;
    struct fake_if *f;
    va_list ap;
    int mode;
    size_t n;

    if (strncmp(path, SYSFS_NET, strlen(SYSFS_NET))) {
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
        return __real_open(path, flags, mode);
    }

    path += strlen(SYSFS_NET);
    rest = strchr(path, '/');
    assert_non_null(rest);
    n = rest - path;
    assert_true(n < IFNAMSIZ);
    memcpy(name, path, n);
    name[n] = 0;
    f = by_name(name);
    assert_non_null(f);

    if (!strcmp(rest, "/brport/flush"))
        f->num_flush++;
    else if (!strcmp(rest, "/bridge/ageing_time"))
        f->num_set_ageing++;
    else
        fail_msg("unexpected sysfs path %s", rest);

    return __real_open("/dev/null", O_WRONLY);
}

/* log.h, ctl_socket_server.h */

void Dprintf(int level, const char *fmt, ...)
{
    char logbuf[LOG_STRING_LEN];
    va_list ap;

    if (level == LOG_LEVEL_ERROR)
        num_errors++;
    if (level > log_level)
        return;

    va_start(ap, fmt);
    vsnprintf(logbuf, sizeof(logbuf), fmt, ap);
    va_end(ap);
    printf("%s\n", logbuf);
}

void _ctl_err_log(char *fmt, ...)
{
}
