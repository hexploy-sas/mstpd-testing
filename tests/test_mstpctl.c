/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * mstpctl argument parsing: yes/no, enums, numbers and argument counts.
 * ctl_main.c is included with main() renamed; the daemon connection is
 * replaced by a recorder of the last request (tests/ctl_fake.c); exit() and
 * if_nametoindex() are intercepted (-Wl,--wrap=exit,--wrap=if_nametoindex).
 *   bridge br0 (index 10), port p1 (index 11)
 */

#define main mstpctl_main
#include "ctl_main.c"
#undef main

#include <setjmp.h>
#include <cmocka.h>

#include "ctl_fake.h"

#define BR CTL_FAKE_BR
#define P1 CTL_FAKE_P1
#define EXITED 1000

static jmp_buf exit_jmp;
static bool exit_armed;

void __real_exit(int status);

void __wrap_exit(int status)
{
    if (exit_armed)
        longjmp(exit_jmp, EXITED + status);
    __real_exit(status);
}

/* run mstpctl with a NULL-terminated argument list; returns its exit code,
 * EXITED + status if it called exit() */
static int run_argv(char **argv)
{
    int argc = 0, r;

    while (argv[argc])
        argc++;
    optind = 0;
    format = FORMAT_PLAIN;
    ctl_fake_reset();

    r = setjmp(exit_jmp);
    if (r) {
        exit_armed = false;
        return r;
    }
    exit_armed = true;
    r = mstpctl_main(argc, argv);
    exit_armed = false;
    return r;
}

#define run(...) run_argv((char *[]){ "mstpctl", __VA_ARGS__, NULL })

static CIST_PortConfig *port_cfg(void)
{
    struct set_cist_port_config_IN *in = ctl_fake_last_in();

    assert_int_equal(ctl_fake_num_msgs(), 1);
    assert_int_equal(ctl_fake_last_cmd(), CMD_CODE_set_cist_port_config);
    assert_int_equal(in->br_index, BR);
    assert_int_equal(in->port_index, P1);
    return &in->cfg;
}

static CIST_BridgeConfig *bridge_cfg(void)
{
    struct set_cist_bridge_config_IN *in = ctl_fake_last_in();

    assert_int_equal(ctl_fake_num_msgs(), 1);
    assert_int_equal(ctl_fake_last_cmd(), CMD_CODE_set_cist_bridge_config);
    assert_int_equal(in->br_index, BR);
    return &in->cfg;
}

/* yes/no commands: each sets its own field only */
static void ctl_yes_no(void **state)
{
    CIST_PortConfig *c;

    assert_int_equal(run("setportadminedge", "br0", "p1", "yes"), 0);
    c = port_cfg();
    assert_true(c->set_admin_edge_port && c->admin_edge_port);
    assert_false(c->set_auto_edge_port);

    assert_int_equal(run("setportadminedge", "br0", "p1", "no"), 0);
    c = port_cfg();
    assert_true(c->set_admin_edge_port && !c->admin_edge_port);

    assert_int_equal(run("setportautoedge", "br0", "p1", "yes"), 0);
    assert_true(port_cfg()->set_auto_edge_port && port_cfg()->auto_edge_port);
    assert_int_equal(run("setportrestrrole", "br0", "p1", "yes"), 0);
    assert_true(port_cfg()->set_restricted_role && port_cfg()->restricted_role);
    assert_int_equal(run("setportrestrtcn", "br0", "p1", "yes"), 0);
    assert_true(port_cfg()->set_restricted_tcn && port_cfg()->restricted_tcn);
    assert_int_equal(run("setbpduguard", "br0", "p1", "yes"), 0);
    assert_true(port_cfg()->set_bpdu_guard_port && port_cfg()->bpdu_guard_port);
    assert_int_equal(run("setportbpdufilter", "br0", "p1", "yes"), 0);
    assert_true(port_cfg()->set_bpdu_filter_port && port_cfg()->bpdu_filter_port);
    assert_int_equal(run("setportnetwork", "br0", "p1", "yes"), 0);
    assert_true(port_cfg()->set_network_port && port_cfg()->network_port);
    assert_int_equal(run("setportdonttxmt", "br0", "p1", "yes"), 0);
    assert_true(port_cfg()->set_dont_txmt && port_cfg()->dont_txmt);

    /* anything else is refused before any request */
    assert_int_equal(run("setportadminedge", "br0", "p1", "on"), EXITED + 1);
    assert_int_equal(run("setportadminedge", "br0", "p1", "YES"), EXITED + 1);
    assert_int_equal(run("setportadminedge", "br0", "p1", "1"), EXITED + 1);
    assert_int_equal(ctl_fake_num_msgs(), 0);
}

/* enums: point-to-point and protocol version */
static void ctl_enums(void **state)
{
    assert_int_equal(run("setportp2p", "br0", "p1", "yes"), 0);
    assert_int_equal(port_cfg()->admin_p2p, p2pForceTrue);
    assert_true(port_cfg()->set_admin_p2p);
    assert_int_equal(run("setportp2p", "br0", "p1", "no"), 0);
    assert_int_equal(port_cfg()->admin_p2p, p2pForceFalse);
    assert_int_equal(run("setportp2p", "br0", "p1", "auto"), 0);
    assert_int_equal(port_cfg()->admin_p2p, p2pAuto);
    assert_int_equal(run("setportp2p", "br0", "p1", "maybe"), EXITED + 1);
    assert_int_equal(ctl_fake_num_msgs(), 0);

    assert_int_equal(run("setforcevers", "br0", "stp"), 0);
    assert_int_equal(bridge_cfg()->protocol_version, protoSTP);
    assert_true(bridge_cfg()->set_protocol_version);
    assert_int_equal(run("setforcevers", "br0", "rstp"), 0);
    assert_int_equal(bridge_cfg()->protocol_version, protoRSTP);
    assert_int_equal(run("setforcevers", "br0", "mstp"), 0);
    assert_int_equal(bridge_cfg()->protocol_version, protoMSTP);
    assert_int_equal(run("setforcevers", "br0", "2"), EXITED + 1);
    assert_int_equal(ctl_fake_num_msgs(), 0);
}

/* unsigned numbers: decimal, hex, octal; garbage, negative, empty and
 * out of range refused */
static void ctl_numbers(void **state)
{
    assert_int_equal(run("setportpathcost", "br0", "p1", "20000"), 0);
    assert_int_equal(port_cfg()->admin_external_port_path_cost, 20000);
    assert_true(port_cfg()->set_admin_external_port_path_cost);
    assert_int_equal(run("setportpathcost", "br0", "p1", "0x10"), 0);
    assert_int_equal(port_cfg()->admin_external_port_path_cost, 16);
    assert_int_equal(run("setportpathcost", "br0", "p1", "010"), 0);
    assert_int_equal(port_cfg()->admin_external_port_path_cost, 8);
    assert_int_equal(run("setportpathcost", "br0", "p1", "0"), 0);
    assert_int_equal(port_cfg()->admin_external_port_path_cost, 0);

    assert_int_equal(run("setportpathcost", "br0", "p1", "12abc"), EXITED + 1);
    /* a negative value is taken as an unknown option: help, exit code 1 */
    assert_int_equal(run("setportpathcost", "br0", "p1", "-1"), 1);
    assert_int_equal(run("setportpathcost", "br0", "p1", ""), EXITED + 1);
    assert_int_equal(run("setportpathcost", "br0", "p1", "4294967296"),
                     EXITED + 1);
    assert_int_equal(ctl_fake_num_msgs(), 0);

    assert_int_equal(run("settxholdcount", "br0", "6"), 0);
    assert_int_equal(bridge_cfg()->tx_hold_count, 6);
    assert_int_equal(run("setageing", "br0", "300"), 0);
    assert_int_equal(bridge_cfg()->bridge_ageing_time, 300);
}

/* 8-bit values above UINT8_MAX are sent as UINT8_MAX (then rejected by the
 * daemon) */
static void ctl_byte_values_clamped(void **state)
{
    struct set_msti_bridge_config_IN *in = ctl_fake_last_in();

    assert_int_equal(run("setmaxage", "br0", "20"), 0);
    assert_int_equal(bridge_cfg()->bridge_max_age, 20);
    assert_int_equal(run("setmaxage", "br0", "300"), 0);
    assert_int_equal(bridge_cfg()->bridge_max_age, UINT8_MAX);
    assert_int_equal(run("setfdelay", "br0", "256"), 0);
    assert_int_equal(bridge_cfg()->bridge_forward_delay, UINT8_MAX);
    assert_int_equal(run("sethello", "br0", "2"), 0);
    assert_int_equal(bridge_cfg()->bridge_hello_time, 2);

    assert_int_equal(run("settreeprio", "br0", "0", "8"), 0);
    assert_int_equal(ctl_fake_last_cmd(), CMD_CODE_set_msti_bridge_config);
    assert_int_equal(in->mstid, 0);
    assert_int_equal(in->bridge_priority, 8);
    assert_int_equal(run("settreeprio", "br0", "0", "300"), 0);
    assert_int_equal(in->bridge_priority, UINT8_MAX);
}

/* tree ID 0..MAX_MSTID, decimal only */
static void ctl_tree_id(void **state)
{
    struct set_msti_bridge_config_IN *in = ctl_fake_last_in();

    assert_int_equal(run("settreeprio", "br0", "4094", "8"), 0);
    assert_int_equal(in->mstid, MAX_MSTID);
    assert_int_equal(run("settreeprio", "br0", "4095", "8"), -1);
    assert_int_equal(ctl_fake_num_msgs(), 0);
    assert_int_equal(run("settreeprio", "br0", "-1", "8"), 1);
    assert_int_equal(run("settreeprio", "br0", "x", "8"), -1);
    assert_int_equal(ctl_fake_num_msgs(), 0);
}

/* argument count, unknown command, unknown interface, options */
static void ctl_command_line(void **state)
{
    assert_int_equal(run("setportpathcost", "br0", "p1"), 1);
    assert_int_equal(run("setportpathcost", "br0", "p1", "1", "2"), 1);
    assert_int_equal(run("setnothing", "br0"), 1);
    assert_int_equal(ctl_fake_num_msgs(), 0);

    assert_int_equal(run("setportpathcost", "br9", "p1", "1"), EXITED + 1);
    assert_int_equal(run("setportpathcost", "br0", "p9", "1"), EXITED + 1);
    assert_int_equal(ctl_fake_num_msgs(), 0);

    assert_int_equal(run("-f", "json", "setmaxage", "br0", "20"), 0);
    assert_int_equal(format, FORMAT_JSON);
    assert_int_equal(run("-f", "xml", "setmaxage", "br0", "20"), 1);
    assert_int_equal(ctl_fake_num_msgs(), 0);
}

int main(int argc, char *argv[])
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(ctl_yes_no),
        cmocka_unit_test(ctl_enums),
        cmocka_unit_test(ctl_numbers),
        cmocka_unit_test(ctl_byte_values_clamped),
        cmocka_unit_test(ctl_tree_id),
        cmocka_unit_test(ctl_command_line),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
