/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <net/if.h>
#include <poll.h>
#include <arpa/inet.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <linux/if_bridge.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/neighbour.h>
#include <linux/if_link.h>
#include <linux/rtnetlink.h>
#include <linux/veth.h>

#include "sys.h"
#include "log.h"
#include "mstp.h"

/* Bridge STP mode attribute (kernel >= 7.1), not in older UAPI headers */
#define SYS_IFLA_BR_STP_MODE 50
#define SYS_BR_STP_MODE_USER 1

#define MAX_ARGS 16
#define CTL_RETRY_MS 3000

static bool ready;
static const char *skip_reason = "environment not initialised";
static int base_netns = -1;
static struct rtnl_handle base_rth;
static char logdir[200];

/* logging for libnetlink.c */
int log_level = LOG_LEVEL_ERROR;
int ctl_in_handler = 0;

void _ctl_err_log(char *fmt, ...)
{
}

void Dprintf(int level, const char *fmt, ...)
{
    va_list ap;

    if (level > log_level)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static int write_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY);
    ssize_t n;

    if (fd < 0)
        return -1;
    n = write(fd, text, strlen(text));
    close(fd);
    return n == (ssize_t)strlen(text) ? 0 : -1;
}

static int not_ready(const char *reason)
{
    skip_reason = reason;
    return -1;
}

int sys_init(void)
{
    const char *dir = getenv("FTEST_LOGDIR");
    int flags = CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWPID;
    uid_t uid = getuid();
    gid_t gid = getgid();
    char map[64];
    pid_t pid;
    int status;

    if (geteuid() != 0)
        flags |= CLONE_NEWUSER;
    if (unshare(flags))
        return not_ready("cannot create namespaces (unprivileged user "
                         "namespaces disabled?)");
    if (flags & CLONE_NEWUSER) {
        snprintf(map, sizeof(map), "0 %u 1", uid);
        if (write_file("/proc/self/setgroups", "deny")
            || write_file("/proc/self/uid_map", map))
            return not_ready("cannot map user");
        snprintf(map, sizeof(map), "0 %u 1", gid);
        if (write_file("/proc/self/gid_map", map))
            return not_ready("cannot map group");
    }

    /* the child is pid 1 of the new pid namespace: when it exits, all
     * mstpd and mstpctl processes of the test go with it */
    pid = fork();
    if (pid < 0)
        return not_ready("fork failed");
    if (pid > 0) {
        if (waitpid(pid, &status, 0) < 0)
            exit(1);
        exit(WIFEXITED(status) ? WEXITSTATUS(status) : 1);
    }

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL)
        || mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC,
                 NULL))
        return not_ready("cannot mount /proc");
    base_netns = open("/proc/self/ns/net", O_RDONLY | O_CLOEXEC);
    if (base_netns < 0 || rtnl_open(&base_rth, 0) < 0)
        return not_ready("cannot open network namespace");

    snprintf(logdir, sizeof(logdir), "%s", dir ? dir : "ftest-logs");
    if (mkdir(logdir, 0755) && errno != EEXIST)
        return not_ready("cannot create log directory");

    ready = true;
    return 0;
}

const char *sys_skip_reason(void)
{
    return ready ? NULL : skip_reason;
}

void sys_require(void)
{
    if (!ready) {
        print_message("skipped: %s\n", skip_reason);
        skip();
    }
}

long sys_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

long sys_wait(bool (*pred)(void *), void *arg, long timeout_ms)
{
    long start = sys_now_ms(), now;

    for (;;) {
        now = sys_now_ms();
        if (pred(arg))
            return now - start;
        if (now - start >= timeout_ms)
            return -1;
        usleep(SYS_POLL_MS * 1000);
    }
}

/* rtnetlink requests in a DUT namespace */

struct link_req {
    struct nlmsghdr n;
    struct ifinfomsg i;
    char buf[1024];
};

static void req_init(struct link_req *r, int type, int flags)
{
    memset(r, 0, sizeof(*r));
    r->n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    r->n.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | flags;
    r->n.nlmsg_type = type;
    r->i.ifi_family = AF_UNSPEC;
}

static int rth_ifindex(struct rtnl_handle *rth, const char *name)
{
    struct link_req r;
    struct nlmsghdr *answer = NULL;
    int index;

    req_init(&r, RTM_GETLINK, 0);
    r.n.nlmsg_flags &= ~NLM_F_ACK;
    addattr_l(&r.n, sizeof(r), IFLA_IFNAME, name, strlen(name) + 1);
    if (rtnl_talk(rth, &r.n, &answer) < 0)
        return -1;
    index = ((struct ifinfomsg *)NLMSG_DATA(answer))->ifi_index;
    free(answer);
    return index;
}

static int get_ifindex(struct dut *d, const char *name)
{
    return rth_ifindex(&d->rth, name);
}

static int rth_set_link(struct rtnl_handle *rth, const char *ifname, bool up,
                        int master)
{
    struct link_req r;
    int index = rth_ifindex(rth, ifname);

    if (index <= 0)
        return -1;
    req_init(&r, RTM_SETLINK, 0);
    r.i.ifi_index = index;
    r.i.ifi_change = IFF_UP;
    r.i.ifi_flags = up ? IFF_UP : 0;
    if (master > 0)
        addattr32(&r.n, sizeof(r), IFLA_MASTER, master);
    return rtnl_talk(rth, &r.n, NULL);
}

static int set_link(struct dut *d, const char *ifname, bool up, int master)
{
    return rth_set_link(&d->rth, ifname, up, master);
}

int dut_set_link(struct dut *d, const char *ifname, bool up)
{
    return set_link(d, ifname, up, 0);
}

int dut_create(struct dut *d, const char *name, unsigned char mac_last)
{
    unsigned char mac[6] = { 0x02, 0, 0, 0, 0, mac_last };
    struct rtattr *linkinfo, *data;
    struct link_req r;

    memset(d, 0, sizeof(*d));
    d->netns = -1;
    snprintf(d->name, sizeof(d->name), "%s", name);
    snprintf(d->log, sizeof(d->log), "%s/%s.log", logdir, name);
    unlink(d->log);

    /* new namespace: keep a reference and a netlink socket in it */
    if (unshare(CLONE_NEWNET))
        return -1;
    d->netns = open("/proc/self/ns/net", O_RDONLY | O_CLOEXEC);
    if (d->netns < 0 || rtnl_open(&d->rth, 0) < 0
        || setns(base_netns, CLONE_NEWNET))
        return -1;

    if (set_link(d, "lo", true, 0))
        return -1;

    req_init(&r, RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL);
    addattr_l(&r.n, sizeof(r), IFLA_IFNAME, "br0", 4);
    addattr_l(&r.n, sizeof(r), IFLA_ADDRESS, mac, sizeof(mac));
    linkinfo = addattr_nest(&r.n, sizeof(r), IFLA_LINKINFO);
    addattr_l(&r.n, sizeof(r), IFLA_INFO_KIND, "bridge", strlen("bridge"));
    data = addattr_nest(&r.n, sizeof(r), IFLA_INFO_DATA);
    addattr32(&r.n, sizeof(r), SYS_IFLA_BR_STP_MODE, SYS_BR_STP_MODE_USER);
    addattr32(&r.n, sizeof(r), IFLA_BR_STP_STATE, 1);
    addattr_nest_end(&r.n, data);
    addattr_nest_end(&r.n, linkinfo);
    if (rtnl_talk(&d->rth, &r.n, NULL) < 0)
        return -1;

    return set_link(d, "br0", true, 0);
}

/* veth pair: aport in the namespace of rth, bport in peer_netns */
static int veth_create(struct rtnl_handle *rth, const char *aport,
                       const char *bport, int peer_netns,
                       const unsigned char *peer_mac)
{
    struct rtattr *linkinfo, *data, *peer;
    struct link_req r;

    req_init(&r, RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL);
    addattr_l(&r.n, sizeof(r), IFLA_IFNAME, aport, strlen(aport) + 1);
    linkinfo = addattr_nest(&r.n, sizeof(r), IFLA_LINKINFO);
    addattr_l(&r.n, sizeof(r), IFLA_INFO_KIND, "veth", strlen("veth"));
    data = addattr_nest(&r.n, sizeof(r), IFLA_INFO_DATA);
    peer = addattr_nest(&r.n, sizeof(r), VETH_INFO_PEER);
    r.n.nlmsg_len += sizeof(struct ifinfomsg);
    addattr_l(&r.n, sizeof(r), IFLA_IFNAME, bport, strlen(bport) + 1);
    if (peer_mac)
        addattr_l(&r.n, sizeof(r), IFLA_ADDRESS, peer_mac, ETH_ALEN);
    addattr32(&r.n, sizeof(r), IFLA_NET_NS_FD, peer_netns);
    addattr_nest_end(&r.n, peer);
    addattr_nest_end(&r.n, data);
    addattr_nest_end(&r.n, linkinfo);
    return rtnl_talk(rth, &r.n, NULL) < 0 ? -1 : 0;
}

/* enslave a new port to br0, down until dut_start() */
static int add_port(struct dut *d, const char *port)
{
    int br = get_ifindex(d, "br0");

    if (br <= 0 || d->nports >= DUT_MAX_PORTS
        || set_link(d, port, false, br))
        return -1;
    snprintf(d->ports[d->nports++], sizeof(d->ports[0]), "%s", port);
    return 0;
}

int dut_link(struct dut *a, const char *aport, struct dut *b, const char *bport)
{
    if (veth_create(&a->rth, aport, bport, b->netns, NULL)
        || add_port(a, aport) || add_port(b, bport))
        return -1;
    return 0;
}

/* child process in the DUT namespace, with a sysfs that shows it */
static void enter_dut(struct dut *d)
{
    if (setns(d->netns, CLONE_NEWNET) || unshare(CLONE_NEWNS)
        || mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL)
        || mount("sysfs", "/sys", "sysfs", 0, NULL))
        _exit(126);
}

static const char *bin(const char *env, const char *def)
{
    const char *p = getenv(env);

    return p ? p : def;
}

int dut_ctl(struct dut *d, char *out, size_t outlen, ...)
{
    const char *argv[MAX_ARGS + 2];
    int pipefd[2], argc = 0, status, fd;
    size_t len = 0;
    ssize_t n;
    va_list ap;
    pid_t pid;
    char sink[256];

    argv[argc++] = "mstpctl";
    va_start(ap, outlen);
    while (argc <= MAX_ARGS && (argv[argc] = va_arg(ap, const char *)))
        argc++;
    va_end(ap);
    argv[argc] = NULL;

    if (pipe(pipefd))
        return -1;
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        enter_dut(d);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        /* mstpctl errors go to the DUT log */
        fd = open(d->log, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0)
            dup2(fd, STDERR_FILENO);
        execv(bin("MSTPCTL", "./mstpctl"), (char *const *)argv);
        _exit(127);
    }
    close(pipefd[1]);
    while ((n = read(pipefd[0], out && len + 1 < outlen ? out + len : sink,
                     out && len + 1 < outlen ? outlen - len - 1
                                             : sizeof(sink))) > 0)
        if (out && len + 1 < outlen)
            len += n;
    close(pipefd[0]);
    if (out && outlen)
        out[len] = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int dut_start(struct dut *d)
{
    char maxage[8], fdelay[8];
    long start;
    int fd, i;

    d->mstpd = fork();
    if (d->mstpd < 0)
        return -1;
    if (d->mstpd == 0) {
        enter_dut(d);
        fd = open(d->log, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
        }
        execl(bin("MSTPD", "./mstpd"), "mstpd", "-d", "-v",
              bin("FTEST_MSTPD_LOGLEVEL", "2"), (char *)NULL);
        _exit(127);
    }

    /* the control socket is up once addbridge answers */
    start = sys_now_ms();
    while (dut_ctl(d, NULL, 0, "addbridge", "br0", (char *)NULL)) {
        if (sys_now_ms() - start > CTL_RETRY_MS)
            return -1;
        usleep(50000);
    }

    /* Max Age before Forward Delay: 2 * (Forward Delay - 1) >= Max Age */
    snprintf(maxage, sizeof(maxage), "%d", SYS_MAX_AGE);
    snprintf(fdelay, sizeof(fdelay), "%d", SYS_FWD_DELAY);
    if (dut_ctl(d, NULL, 0, "setmaxage", "br0", maxage, (char *)NULL)
        || dut_ctl(d, NULL, 0, "setfdelay", "br0", fdelay, (char *)NULL))
        return -1;

    for (i = 0; i < d->nports; i++)
        if (set_link(d, d->ports[i], true, 0))
            return -1;
    return 0;
}

void dut_destroy(struct dut *d)
{
    if (d->mstpd > 0) {
        kill(d->mstpd, SIGTERM);
        waitpid(d->mstpd, NULL, 0);
        d->mstpd = 0;
    }
    if (d->netns >= 0) {
        rtnl_close(&d->rth);
        close(d->netns);
        d->netns = -1;
    }
}

/* kernel port state from an AF_BRIDGE link dump */

struct state_query {
    const char *port;
    int state;
};

static int state_filter(struct nlmsghdr *n, void *arg)
{
    struct state_query *q = arg;
    struct ifinfomsg *ifi = NLMSG_DATA(n);
    struct rtattr *tb[IFLA_MAX + 1], *brtb[IFLA_BRPORT_MAX + 1];
    int len = n->nlmsg_len - NLMSG_LENGTH(sizeof(*ifi));

    if (n->nlmsg_type != RTM_NEWLINK || len < 0)
        return 0;
    parse_rtattr_flags(tb, IFLA_MAX, IFLA_RTA(ifi), len, NLA_F_NESTED);
    if (!tb[IFLA_IFNAME] || strcmp(rta_getattr_str(tb[IFLA_IFNAME]), q->port)
        || !tb[IFLA_PROTINFO])
        return 0;
    parse_rtattr_nested(brtb, IFLA_BRPORT_MAX, tb[IFLA_PROTINFO]);
    if (brtb[IFLA_BRPORT_STATE])
        q->state = rta_getattr_u8(brtb[IFLA_BRPORT_STATE]);
    return 0;
}

int dut_port_state(struct dut *d, const char *port)
{
    struct state_query q = { .port = port, .state = -1 };

    if (rtnl_linkdump_req(&d->rth, AF_BRIDGE) < 0
        || rtnl_dump_filter(&d->rth, state_filter, &q) < 0)
        return -1;
    return q.state;
}

/* value of "key":"value" in mstpctl JSON output */
static int json_value(const char *json, const char *key, char *val, size_t len)
{
    char pattern[64];
    const char *p, *end;

    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    p = strstr(json, pattern);
    if (!p)
        return -1;
    p += strlen(pattern);
    end = strchr(p, '"');
    if (!end || (size_t)(end - p) >= len)
        return -1;
    memcpy(val, p, end - p);
    val[end - p] = 0;
    return 0;
}

int dut_port_param(struct dut *d, const char *port, const char *key,
                   char *val, size_t len)
{
    char out[4096];

    if (dut_ctl(d, out, sizeof(out), "-f", "json", "showportdetail", "br0",
                port, (char *)NULL))
        return -1;
    return json_value(out, key, val, len);
}

int dut_port_role(struct dut *d, const char *port, char *role, size_t len)
{
    return dut_port_param(d, port, "role", role, len);
}

int dut_bridge_ids(struct dut *d, char *bridge_id, char *root_id, size_t len)
{
    char out[4096];

    if (dut_ctl(d, out, sizeof(out), "-f", "json", "showbridge", "br0",
                (char *)NULL))
        return -1;
    if (json_value(out, "bridge-id", bridge_id, len)
        || json_value(out, "designated-root", root_id, len))
        return -1;
    return 0;
}

bool dut_mstpd_alive(struct dut *d)
{
    return d->mstpd > 0 && waitpid(d->mstpd, NULL, WNOHANG) == 0;
}

/* interface flags */

struct flags_query {
    const char *ifname;
    int flags;
};

static int flags_filter(struct nlmsghdr *n, void *arg)
{
    struct flags_query *q = arg;
    struct ifinfomsg *ifi = NLMSG_DATA(n);
    struct rtattr *tb[IFLA_MAX + 1];
    int len = n->nlmsg_len - NLMSG_LENGTH(sizeof(*ifi));

    if (n->nlmsg_type != RTM_NEWLINK || len < 0)
        return 0;
    parse_rtattr_flags(tb, IFLA_MAX, IFLA_RTA(ifi), len, NLA_F_NESTED);
    if (tb[IFLA_IFNAME] && !strcmp(rta_getattr_str(tb[IFLA_IFNAME]), q->ifname))
        q->flags = ifi->ifi_flags;
    return 0;
}

int dut_link_flags(struct dut *d, const char *ifname)
{
    struct flags_query q = { .ifname = ifname, .flags = -1 };

    if (rtnl_linkdump_req(&d->rth, AF_UNSPEC) < 0
        || rtnl_dump_filter(&d->rth, flags_filter, &q) < 0)
        return -1;
    return q.flags;
}

/* forwarding database */

struct fdb_query {
    int ifindex;
    const unsigned char *mac;
    bool found;
};

static int fdb_filter(struct nlmsghdr *n, void *arg)
{
    struct fdb_query *q = arg;
    struct ndmsg *ndm = NLMSG_DATA(n);
    struct rtattr *tb[NDA_MAX + 1];
    int len = n->nlmsg_len - NLMSG_LENGTH(sizeof(*ndm));

    if (n->nlmsg_type != RTM_NEWNEIGH || len < 0 || ndm->ndm_family != AF_BRIDGE)
        return 0;
    parse_rtattr(tb, NDA_MAX, (struct rtattr *)((char *)ndm
                 + NLMSG_ALIGN(sizeof(*ndm))), len);
    if (ndm->ndm_ifindex == q->ifindex && tb[NDA_LLADDR]
        && RTA_PAYLOAD(tb[NDA_LLADDR]) == ETH_ALEN
        && !memcmp(RTA_DATA(tb[NDA_LLADDR]), q->mac, ETH_ALEN))
        q->found = true;
    return 0;
}

int dut_fdb_has(struct dut *d, const char *port, const unsigned char *mac)
{
    struct fdb_query q = { .mac = mac };

    q.ifindex = get_ifindex(d, port);
    if (q.ifindex <= 0 || rtnl_neighdump_req(&d->rth, AF_BRIDGE, NULL) < 0
        || rtnl_dump_filter(&d->rth, fdb_filter, &q) < 0)
        return -1;
    return q.found;
}

/* virtual neighbours: the far end of a veth pair, in the harness namespace,
 * with an AF_PACKET socket */

static const unsigned char stp_group[ETH_ALEN] = {
    0x01, 0x80, 0xc2, 0x00, 0x00, 0x00
};

#define LLC_HLEN 3
#define FRAME_HLEN (ETH_HLEN + LLC_HLEN)
#define MIN_FRAME 60

int vnb_link(struct vnb *n, const char *name, unsigned char mac_last,
             struct dut *d, const char *port)
{
    struct sockaddr_ll sll;

    memset(n, 0, sizeof(*n));
    n->fd = -1;
    snprintf(n->ifname, sizeof(n->ifname), "%s", name);
    n->mac[0] = 0x02;
    n->mac[4] = 0x01;
    n->mac[5] = mac_last;

    if (veth_create(&d->rth, port, name, base_netns, n->mac)
        || add_port(d, port) || rth_set_link(&base_rth, name, true, 0))
        return -1;

    n->fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL));
    if (n->fd < 0)
        return -1;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex = rth_ifindex(&base_rth, name);
    return bind(n->fd, (struct sockaddr *)&sll, sizeof(sll));
}

int vnb_send_frame(struct vnb *n, const void *frame, size_t len)
{
    unsigned char buf[ETH_FRAME_LEN] = { 0 };

    if (len > sizeof(buf))
        return -1;
    memcpy(buf, frame, len);
    if (len < MIN_FRAME)
        len = MIN_FRAME;
    return send(n->fd, buf, len, 0) == (ssize_t)len ? 0 : -1;
}

int vnb_send(struct vnb *n, const void *bpdu, size_t len)
{
    unsigned char f[ETH_FRAME_LEN];

    if (len + FRAME_HLEN > sizeof(f))
        return -1;
    memcpy(f, stp_group, ETH_ALEN);
    memcpy(f + ETH_ALEN, n->mac, ETH_ALEN);
    f[12] = (len + LLC_HLEN) >> 8;
    f[13] = (len + LLC_HLEN) & 0xff;
    f[14] = 0x42;
    f[15] = 0x42;
    f[16] = 0x03;
    memcpy(f + FRAME_HLEN, bpdu, len);
    return vnb_send_frame(n, f, FRAME_HLEN + len);
}

int vnb_send_periodic(struct vnb *n, const void *bpdu, size_t len,
                      int period_ms)
{
    vnb_stop(n);
    if (vnb_send(n, bpdu, len))
        return -1;
    n->sender = fork();
    if (n->sender < 0)
        return -1;
    if (n->sender == 0) {
        for (;;) {
            usleep(period_ms * 1000);
            vnb_send(n, bpdu, len);
        }
    }
    return 0;
}

void vnb_stop(struct vnb *n)
{
    if (n->sender > 0) {
        kill(n->sender, SIGKILL);
        waitpid(n->sender, NULL, 0);
        n->sender = 0;
    }
}

int vnb_recv(struct vnb *n, void *bpdu, size_t len, long timeout_ms)
{
    unsigned char f[ETH_FRAME_LEN];
    struct sockaddr_ll sll;
    struct pollfd pfd = { .fd = n->fd, .events = POLLIN };
    long end = sys_now_ms() + timeout_ms, left;
    socklen_t sl;
    ssize_t r;
    size_t l;

    /* at least one poll, so that a zero timeout reads what is queued */
    for (;;) {
        left = end - sys_now_ms();
        if (poll(&pfd, 1, left > 0 ? left : 0) <= 0)
            break;
        sl = sizeof(sll);
        r = recvfrom(n->fd, f, sizeof(f), 0, (struct sockaddr *)&sll, &sl);
        if (r < FRAME_HLEN || sll.sll_pkttype == PACKET_OUTGOING
            || memcmp(f, stp_group, ETH_ALEN) || f[14] != 0x42
            || f[15] != 0x42 || f[16] != 0x03)
            continue;
        l = ((f[12] << 8) | f[13]) - LLC_HLEN;
        if (l > (size_t)r - FRAME_HLEN)
            continue;
        if (l > len)
            l = len;
        memcpy(bpdu, f + FRAME_HLEN, l);
        return l;
    }
    return -1;
}

void vnb_destroy(struct vnb *n)
{
    struct link_req r;
    int index;

    vnb_stop(n);
    if (n->fd >= 0) {
        close(n->fd);
        n->fd = -1;
    }
    /* namespaces are freed asynchronously: delete the veth pair now so
     * that the next test can reuse the name */
    index = n->ifname[0] ? rth_ifindex(&base_rth, n->ifname) : -1;
    if (index > 0) {
        req_init(&r, RTM_DELLINK, 0);
        r.i.ifi_index = index;
        if (rtnl_talk(&base_rth, &r.n, NULL) < 0)
            fprintf(stderr, "cannot delete %s\n", n->ifname);
    }
}

void vnb_make_bpdu(void *buf, unsigned int flags, uint16_t root_prio,
                   unsigned char root_mac_last, uint32_t cost,
                   uint16_t bridge_prio, unsigned char bridge_mac_last)
{
    bpdu_t *b = buf;

    memset(b, 0, sizeof(*b));
    b->protocolVersion = protoRSTP;
    b->bpduType = bpduTypeRST;
    b->flags = flags;
    b->cistRootID.s.priority = htons(root_prio);
    b->cistRootID.s.mac_address[0] = 0x02;
    b->cistRootID.s.mac_address[5] = root_mac_last;
    b->cistExtRootPathCost = htonl(cost);
    b->cistRRootID.s.priority = htons(bridge_prio);
    b->cistRRootID.s.mac_address[0] = 0x02;
    b->cistRRootID.s.mac_address[5] = bridge_mac_last;
    b->cistPortID = htons(PORT_ID(PORT_PRIO_DEFAULT, 1));
    b->MaxAge[0] = SYS_MAX_AGE;
    b->HelloTime[0] = HELLO_TIME;
    b->ForwardDelay[0] = SYS_FWD_DELAY;
}
