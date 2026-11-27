/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ftest-sys: control socket access (1b7fbe2): configuration requests need
 * root, status requests do not. mstpctl runs as another uid, so the tests
 * need real root (make check-sys-vm) and skip in a user namespace.
 *   dut0 e1 <-> neighbour A
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "sys.h"

#define NOBODY 65534
#define PATH_COST 1234

struct topo {
    struct dut d;
    struct vnb a;
    char mstpctl[64];
};

static struct topo topo;

/* copy of mstpctl that another uid can run (the build directory may not be
 * reachable for it) */
static int copy_mstpctl(char *dst, size_t len)
{
    const char *src = getenv("MSTPCTL") ? getenv("MSTPCTL") : "./mstpctl";
    char buf[65536];
    ssize_t n;
    int in, out;

    snprintf(dst, len, "/tmp/ftest-mstpctl-%d", (int)getpid());
    in = open(src, O_RDONLY);
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (in < 0 || out < 0)
        return -1;
    while ((n = read(in, buf, sizeof(buf))) > 0)
        if (write(out, buf, n) != n)
            return -1;
    close(in);
    return close(out) || chmod(dst, 0755) ? -1 : 0;
}

static int setup(void **state)
{
    struct topo *t = &topo;

    memset(t, 0, sizeof(*t));
    t->d.netns = t->a.fd = -1;
    *state = t;
    if (sys_skip_reason() || !sys_real_root())
        return 0;
    assert_int_equal(copy_mstpctl(t->mstpctl, sizeof(t->mstpctl)), 0);
    setenv("MSTPCTL", t->mstpctl, 1);
    assert_int_equal(dut_create(&t->d, "dut0", 0x10), 0);
    assert_int_equal(vnb_link(&t->a, "nbA", 0xa, &t->d, "e1"), 0);
    assert_int_equal(dut_start(&t->d), 0);
    return 0;
}

static int teardown(void **state)
{
    dut_destroy(&topo.d);
    vnb_destroy(&topo.a);
    if (topo.mstpctl[0])
        unlink(topo.mstpctl);
    return 0;
}

static long port_value(struct dut *d, const char *key)
{
    char val[32];

    assert_int_equal(dut_port_param(d, "e1", key, val, sizeof(val)), 0);
    return strtol(val, NULL, 10);
}

/* Non-root mstpctl: show commands work, set commands are refused and change
 * nothing; root can set */
static void ctl_non_root(void **state)
{
    struct topo *t = *state;
    char cost[16], out[4096];

    sys_require();
    if (!sys_real_root()) {
        print_message("skipped: needs real root to run mstpctl as another "
                      "uid, see make check-sys-vm\n");
        skip();
    }
    snprintf(cost, sizeof(cost), "%d", PATH_COST);

    assert_int_equal(dut_ctl_as(&t->d, NOBODY, out, sizeof(out), "showbridge",
                                "br0", (char *)NULL), 0);
    assert_non_null(strstr(out, "br0"));
    assert_int_equal(dut_ctl_as(&t->d, NOBODY, NULL, 0, "showportdetail",
                                "br0", "e1", (char *)NULL), 0);

    assert_int_not_equal(dut_ctl_as(&t->d, NOBODY, NULL, 0, "setportpathcost",
                                    "br0", "e1", cost, (char *)NULL), 0);
    assert_int_not_equal(dut_ctl_as(&t->d, NOBODY, NULL, 0, "delbridge",
                                    "br0", (char *)NULL), 0);
    /* nothing changed: cost still automatic, bridge still managed */
    assert_int_equal(port_value(&t->d, "admin-external-cost"), 0);
    assert_int_equal(dut_port_param(&t->d, "e1", "enabled", out, sizeof(out)),
                     0);
    assert_string_equal(out, "yes");

    assert_int_equal(dut_ctl(&t->d, NULL, 0, "setportpathcost", "br0", "e1",
                             cost, (char *)NULL), 0);
    assert_int_equal(port_value(&t->d, "admin-external-cost"), PATH_COST);
}

int main(int argc, char *argv[])
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(ctl_non_root, setup, teardown),
    };

    sys_init();
    return cmocka_run_group_tests(tests, NULL, NULL);
}
