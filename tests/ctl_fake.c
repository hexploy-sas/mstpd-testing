/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <string.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include "ctl_socket_client.h"
#include "ctl_fake.h"

#define MAX_IN 16384

static int num_msgs;
static int last_cmd;
static unsigned char last_in[MAX_IN];

void ctl_fake_reset(void)
{
    num_msgs = 0;
    last_cmd = -1;
    memset(last_in, 0, sizeof(last_in));
}

int ctl_fake_num_msgs(void)
{
    return num_msgs;
}

int ctl_fake_last_cmd(void)
{
    return last_cmd;
}

void *ctl_fake_last_in(void)
{
    return last_in;
}

int send_ctl_message(int cmd, void *inbuf, int lin, void *outbuf, int lout,
                     LogString *log, int *res)
{
    assert_true(lin >= 0 && lin <= MAX_IN);
    memcpy(last_in, inbuf, lin);
    memset(outbuf, 0, lout);
    last_cmd = cmd;
    num_msgs++;
    *res = 0;
    return 0;
}

int ctl_client_init(void)
{
    return 0;
}

void ctl_client_cleanup(void)
{
}

unsigned int __wrap_if_nametoindex(const char *name)
{
    if (!strcmp(name, "br0"))
        return CTL_FAKE_BR;
    if (!strcmp(name, "p1"))
        return CTL_FAKE_P1;
    return 0;
}
