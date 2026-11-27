/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Bridge and port configuration ranges (802.1D-2004 17.14, Table 17-1).
 * A rejected configuration returns -1 and changes nothing.
 */

#include <stdio.h>
#include <linux/if_ether.h>
#include <linux/if_bridge.h>
#include <asm/byteorder.h>

#include "mstp.h"
#include "common.h"
#include "stp_params.h"

/* smallest Max Age that breaks 2 * (Forward Delay - 1) >= Max Age */
#define MAX_AGE_TOO_BIG(fwd_delay) (2 * ((fwd_delay) - 1) + 1)

static bridge_t *setup_bridge(void **state, port_t **port)
{
    bridge_t *br;
    port_t *p[1];

    assert_int_equal(alloc_bridge_ports(state, &br, "br0", 0x200000000001,
                                        &p, 1), 0);
    MSTP_IN_set_bridge_enable(br, true);
    if (port)
        *port = p[0];

    return br;
}

static int set_times(bridge_t *br, __u8 max_age, __u8 fwd_delay)
{
    CIST_BridgeConfig cfg = {
        .bridge_max_age = max_age,
        .set_bridge_max_age = true,
        .bridge_forward_delay = fwd_delay,
        .set_bridge_forward_delay = true,
    };

    return MSTP_IN_set_cist_bridge_config(br, &cfg);
}

static void assert_times(bridge_t *br, __u8 max_age, __u8 fwd_delay)
{
    CIST_BridgeStatus s;

    MSTP_IN_get_cist_bridge_status(br, &s);
    assert_int_equal(s.bridge_max_age, max_age);
    assert_int_equal(s.bridge_forward_delay, fwd_delay);
}

/* Max Age and Forward Delay ranges, 2 * (Forward Delay - 1) >= Max Age */
static void bridge_config_times(void **state)
{
    bridge_t *br = setup_bridge(state, NULL);

    /* defaults */
    assert_times(br, MAX_AGE_DEFAULT, FWD_DELAY_DEFAULT);

    /* boundaries accepted */
    assert_int_equal(set_times(br, MAX_AGE_MIN, FWD_DELAY_MIN), 0);
    assert_times(br, MAX_AGE_MIN, FWD_DELAY_MIN);
    assert_int_equal(set_times(br, MAX_AGE_MAX, FWD_DELAY_MAX), 0);
    assert_times(br, MAX_AGE_MAX, FWD_DELAY_MAX);
    assert_int_equal(set_times(br, MAX_AGE_DEFAULT, FWD_DELAY_DEFAULT), 0);

    /* out of range rejected, nothing changed */
    assert_int_equal(set_times(br, MAX_AGE_MIN - 1, FWD_DELAY_DEFAULT), -1);
    assert_int_equal(set_times(br, MAX_AGE_MAX + 1, FWD_DELAY_MAX), -1);
    assert_int_equal(set_times(br, MAX_AGE_MIN, FWD_DELAY_MIN - 1), -1);
    assert_int_equal(set_times(br, MAX_AGE_DEFAULT, FWD_DELAY_MAX + 1), -1);
    assert_times(br, MAX_AGE_DEFAULT, FWD_DELAY_DEFAULT);

    /* 2 * (Forward Delay - 1) >= Max Age */
    assert_false(TIMES_VALID(MAX_AGE_TOO_BIG(FWD_DELAY_MIN), FWD_DELAY_MIN));
    assert_int_equal(set_times(br, MAX_AGE_TOO_BIG(FWD_DELAY_MIN),
                               FWD_DELAY_MIN), -1);
    assert_times(br, MAX_AGE_DEFAULT, FWD_DELAY_DEFAULT);
    assert_int_equal(set_times(br, MAX_AGE_MIN, FWD_DELAY_MIN), 0);
    assert_times(br, MAX_AGE_MIN, FWD_DELAY_MIN);

    /* a single time is checked against the current other time */
    {
        CIST_BridgeConfig cfg = {
            .bridge_max_age = MAX_AGE_TOO_BIG(FWD_DELAY_MIN),
            .set_bridge_max_age = true,
        };
        assert_int_equal(MSTP_IN_set_cist_bridge_config(br, &cfg), -1);
        assert_times(br, MAX_AGE_MIN, FWD_DELAY_MIN);
    }
}

/* Hello Time is fixed (e894644) */
static void bridge_config_hello_time(void **state)
{
    bridge_t *br = setup_bridge(state, NULL);
    CIST_BridgeConfig cfg = { .set_bridge_hello_time = true };
    CIST_BridgeStatus s;

    cfg.bridge_hello_time = HELLO_TIME - 1;
    assert_int_equal(MSTP_IN_set_cist_bridge_config(br, &cfg), -1);
    cfg.bridge_hello_time = HELLO_TIME + 1;
    assert_int_equal(MSTP_IN_set_cist_bridge_config(br, &cfg), -1);
    cfg.bridge_hello_time = HELLO_TIME;
    assert_int_equal(MSTP_IN_set_cist_bridge_config(br, &cfg), 0);

    MSTP_IN_get_cist_bridge_status(br, &s);
    assert_int_equal(s.bridge_hello_time, HELLO_TIME);
}

/* Transmit Hold Count range */
static void bridge_config_tx_hold_count(void **state)
{
    bridge_t *br = setup_bridge(state, NULL);
    CIST_BridgeConfig cfg = { .set_tx_hold_count = true };
    CIST_BridgeStatus s;

    MSTP_IN_get_cist_bridge_status(br, &s);
    assert_int_equal(s.tx_hold_count, TX_HOLD_DEFAULT);

    cfg.tx_hold_count = TX_HOLD_MIN - 1;
    assert_int_equal(MSTP_IN_set_cist_bridge_config(br, &cfg), -1);
    cfg.tx_hold_count = TX_HOLD_MAX + 1;
    assert_int_equal(MSTP_IN_set_cist_bridge_config(br, &cfg), -1);
    MSTP_IN_get_cist_bridge_status(br, &s);
    assert_int_equal(s.tx_hold_count, TX_HOLD_DEFAULT);

    cfg.tx_hold_count = TX_HOLD_MIN;
    assert_int_equal(MSTP_IN_set_cist_bridge_config(br, &cfg), 0);
    MSTP_IN_get_cist_bridge_status(br, &s);
    assert_int_equal(s.tx_hold_count, TX_HOLD_MIN);
    cfg.tx_hold_count = TX_HOLD_MAX;
    assert_int_equal(MSTP_IN_set_cist_bridge_config(br, &cfg), 0);
    MSTP_IN_get_cist_bridge_status(br, &s);
    assert_int_equal(s.tx_hold_count, TX_HOLD_MAX);
}

/* Protocol version: STP, RSTP or MSTP only */
static void bridge_config_protocol_version(void **state)
{
    bridge_t *br = setup_bridge(state, NULL);
    CIST_BridgeConfig cfg = { .set_protocol_version = true };
    CIST_BridgeStatus s;

    /* between STP and RSTP, and above MSTP */
    cfg.protocol_version = protoSTP + 1;
    assert_int_equal(MSTP_IN_set_cist_bridge_config(br, &cfg), -1);
    cfg.protocol_version = protoMSTP + 1;
    assert_int_equal(MSTP_IN_set_cist_bridge_config(br, &cfg), -1);
    MSTP_IN_get_cist_bridge_status(br, &s);
    assert_int_equal(s.protocol_version, protoRSTP);

    cfg.protocol_version = protoSTP;
    assert_int_equal(MSTP_IN_set_cist_bridge_config(br, &cfg), 0);
    MSTP_IN_get_cist_bridge_status(br, &s);
    assert_int_equal(s.protocol_version, protoSTP);
}

/* Bridge and port priority range */
static void bridge_config_priorities(void **state)
{
    port_t *p;
    bridge_t *br = setup_bridge(state, &p);
    MSTI_PortConfig pcfg = { .set_port_priority = true };
    CIST_BridgeStatus s;
    CIST_PortStatus ps;

    assert_int_equal(MSTP_IN_set_msti_bridge_config(GET_CIST_TREE(br),
                                                    PRIO_MAX + 1), -1);
    MSTP_IN_get_cist_bridge_status(br, &s);
    assert_int_equal(GET_PRIORITY_FROM_IDENTIFIER(s.bridge_id),
                     ID_PRIO_BITS(BRIDGE_PRIO_DEFAULT));
    assert_int_equal(MSTP_IN_set_msti_bridge_config(GET_CIST_TREE(br),
                                                    PRIO_MAX), 0);
    MSTP_IN_get_cist_bridge_status(br, &s);
    assert_int_equal(GET_PRIORITY_FROM_IDENTIFIER(s.bridge_id),
                     ID_PRIO_BITS(PRIO_MAX));

    pcfg.port_priority = PRIO_MAX + 1;
    assert_int_equal(MSTP_IN_set_msti_port_config(GET_CIST_PTP_FROM_PORT(p),
                                                  &pcfg), -1);
    MSTP_IN_get_cist_port_status(p, &ps);
    assert_int_equal(GET_PRIORITY_FROM_IDENTIFIER(ps.port_id),
                     ID_PRIO_BITS(PORT_PRIO_DEFAULT));
    pcfg.port_priority = 0;
    assert_int_equal(MSTP_IN_set_msti_port_config(GET_CIST_PTP_FROM_PORT(p),
                                                  &pcfg), 0);
    MSTP_IN_get_cist_port_status(p, &ps);
    assert_int_equal(GET_PRIORITY_FROM_IDENTIFIER(ps.port_id),
                     ID_PRIO_BITS(0));
}

int main(int argc, char *argv[])
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(bridge_config_times, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(bridge_config_hello_time, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(bridge_config_tx_hold_count, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(bridge_config_protocol_version, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(bridge_config_priorities, prepare_test, teardown_test),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
