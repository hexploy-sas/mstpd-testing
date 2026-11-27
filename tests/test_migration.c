/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Port Protocol Migration state machine (802.1D-2004 17.24).
 * One bridge with one port, BPDUs injected and captured on that port.
 */

#include <stdio.h>
#include <linux/if_ether.h>
#include <linux/if_bridge.h>
#include <asm/byteorder.h>

#include "mstp.h"
#include "common.h"
#include "stp_params.h"

#define OBSERVE_TICKS 10

/* Config BPDU from a legacy bridge with an inferior root (lowest priority),
 * so the port under test stays designated and keeps transmitting */
static const bpdu_t inferior_config_bpdu = {
    .protocolIdentifier = 0,
    .protocolVersion = protoSTP,
    .bpduType = bpduTypeConfig,
    .flags = 0,
    .cistRootID.s = {
        .priority = __constant_cpu_to_be16(BRIDGE_PRIO_VALUE(PRIO_MAX)),
        .mac_address = { 0x02, 0x00, 0x00, 0x00, 0x00, 0xff },
    },
    .cistExtRootPathCost = 0,
    .cistRRootID.s = {
        .priority = __constant_cpu_to_be16(BRIDGE_PRIO_VALUE(PRIO_MAX)),
        .mac_address = { 0x02, 0x00, 0x00, 0x00, 0x00, 0xff },
    },
    .cistPortID = __constant_cpu_to_be16(PORT_ID(PORT_PRIO_DEFAULT, 1)),
    .MessageAge = { 0, 0 },
    .MaxAge = { MAX_AGE_DEFAULT, 0 },
    .HelloTime = { HELLO_TIME, 0 },
    .ForwardDelay = { FWD_DELAY_DEFAULT, 0 },
};

static void tick(void **state, int n)
{
    while (n--)
        test_one_second(state);
}

static port_t *setup_port(void **state, protocol_version_t proto)
{
    bridge_t *br;
    port_t *p[1];
    CIST_BridgeConfig cfg = {
        .protocol_version = proto,
        .set_protocol_version = true,
    };

    assert_int_equal(alloc_bridge_ports(state, &br, "br0", 0x200000000001,
                                        &p, 1), 0);
    assert_int_equal(MSTP_IN_set_cist_bridge_config(br, &cfg), 0);
    MSTP_IN_set_bridge_enable(br, true);
    set_port_state(p[0], true, SPEED_1000, DUPLEX_FULL);

    return p[0];
}

static bool send_rstp(port_t *p)
{
    CIST_PortStatus s;

    MSTP_IN_get_cist_port_status(p, &s);
    return s.sendRSTP;
}

static void assert_role_designated(port_t *p)
{
    CIST_PortStatus s;

    MSTP_IN_get_cist_port_status(p, &s);
    assert_int_equal(s.role, roleDesignated);
}

static void assert_last_tx(port_t *p, __u8 version, __u8 type, size_t size)
{
    bpdu_t *bpdu;
    size_t len;

    assert_int_equal(port_last_tx_bpdu(p, &bpdu, &len), 0);
    assert_int_equal(bpdu->protocolVersion, version);
    assert_int_equal(bpdu->bpduType, type);
    assert_int_equal(len, size);
}

/* Bridge forced to STP: the port sends Config BPDUs only, never RST BPDUs */
static void migration_force_stp_tx_config(void **state)
{
    port_t *p = setup_port(state, protoSTP);
    bpdu_t *bpdu;
    size_t len;
    int w, t, sent;

    assert_false(send_rstp(p));

    /* at least one BPDU per hello time, and every BPDU is a Config BPDU */
    for (w = 0; w < OBSERVE_TICKS / HELLO_TIME; w++) {
        sent = 0;
        for (t = 0; t < HELLO_TIME; t++) {
            port_reset_last_tx_bpdu(p);
            tick(state, 1);
            if (port_last_tx_bpdu(p, &bpdu, &len))
                continue;
            assert_last_tx(p, protoSTP, bpduTypeConfig, CONFIG_BPDU_SIZE);
            sent++;
        }
        assert_true(sent > 0);
        assert_false(send_rstp(p));
    }
    assert_role_designated(p);
}

/* RSTP port receiving an STP BPDU after Migrate Time: sendRSTP false, the
 * port sends Config BPDUs, and stays STP once the neighbour is silent */
static void migration_rx_stp_bpdu(void **state)
{
    port_t *p = setup_port(state, protoRSTP);

    tick(state, MIGRATE_TIME + 1);
    assert_true(send_rstp(p));
    assert_last_tx(p, protoRSTP, bpduTypeRST, RST_BPDU_SIZE);

    port_rx_bpdu(p, &inferior_config_bpdu, CONFIG_BPDU_SIZE);
    assert_false(send_rstp(p));

    port_reset_last_tx_bpdu(p);
    tick(state, HELLO_TIME);
    assert_last_tx(p, protoSTP, bpduTypeConfig, CONFIG_BPDU_SIZE);
    assert_role_designated(p);

    /* no RST BPDU received: no return to RSTP without mcheck */
    tick(state, OBSERVE_TICKS);
    assert_false(send_rstp(p));
    assert_last_tx(p, protoSTP, bpduTypeConfig, CONFIG_BPDU_SIZE);
}

/* An STP BPDU received while mdelayWhile runs (port just enabled) is
 * discarded on entry to SENSING; the next one triggers the migration */
static void migration_ignores_stp_bpdu_during_migrate_time(void **state)
{
    port_t *p = setup_port(state, protoRSTP);

    tick(state, 1);
    port_rx_bpdu(p, &inferior_config_bpdu, CONFIG_BPDU_SIZE);
    tick(state, MIGRATE_TIME + HELLO_TIME);
    assert_true(send_rstp(p));
    assert_last_tx(p, protoRSTP, bpduTypeRST, RST_BPDU_SIZE);

    port_rx_bpdu(p, &inferior_config_bpdu, CONFIG_BPDU_SIZE);
    assert_false(send_rstp(p));
}

int main(int argc, char *argv[])
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(migration_force_stp_tx_config, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(migration_rx_stp_bpdu, prepare_test, teardown_test),
        cmocka_unit_test_setup_teardown(migration_ignores_stp_bpdu_during_migrate_time, prepare_test, teardown_test),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
