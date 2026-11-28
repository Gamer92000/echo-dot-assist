/*
 *  Copyright (c) 2019, The OpenThread Authors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 * @brief
 *   This file includes the platform-specific initializers.
 */

#include "platform_qorvo.h"

#include "stdio.h"
#include "stdlib.h"
#include <string.h>
#include <unistd.h>

#include <openthread/tasklet.h>

#include "utils/uart.h"

#include "alarm_qorvo.h"
#include "platform_qorvo.h"
#include "radio_qorvo.h"
#include "random_qorvo.h"
#include "uart_qorvo.h"

void platformUartInit(void);
void gprcpTimedInit(void);
void platformUartProcess(void);

otInstance *localInstance = NULL;

int    gArgumentsCount = 0;
char **gArguments      = NULL;

uint8_t qorvoPlatGotoSleepCheck(void)
{
    bool canGotoSleep = false;

    if (localInstance)
    {
        canGotoSleep = !otTaskletsArePending(localInstance);
    }

    return canGotoSleep;
}

/* hassmic (gprcp): the notice Qorvo's EULA (section 4.2) requires in the program that links its library, in the
 * image itself, so that no copy of gprcp goes without it.  The rest of what the licences ask for is NOTICE beside the
 * program (scripts/notice.py, every update bundle).  Shown on stderr by `gprcp --notice` only: stdin and stdout are
 * the host's Spinel, and rcpbridge starts gprcp without arguments. */
static const char gprcpNotice[] =
    "gprcp: OpenThread's RCP on ot-qorvo's GP712 platform (scripts/gprcp-build.sh).\n"
    "It contains Qorvo's libQorvoGP712_ftd.a, licensed under Qorvo's EULA:\n"
    "Copyright (c) 2017-2021, Qorvo Inc.\n"
    "OpenThread and ot-qorvo: Copyright (c) 2016, The OpenThread Authors (BSD-3-Clause).\n"
    "Mbed TLS: Copyright The Mbed TLS Contributors (Apache-2.0). musl: Copyright (c) 2005-2020 Rich Felker, et al. (MIT).\n"
    "The licences and notices in full: NOTICE beside this program.\n";

void otSysInit(int argc, char *argv[])
{
    if (argc == 2 && strcmp(argv[1], "--notice") == 0)
    {
        ssize_t w = write(STDERR_FILENO, gprcpNotice, sizeof gprcpNotice - 1);
        exit(w == (ssize_t)(sizeof gprcpNotice - 1) ? EXIT_SUCCESS : EXIT_FAILURE);
    }

    gArgumentsCount = argc;
    gArguments      = argv;

    qorvoPlatInit((qorvoPlatGotoSleepCheckCallback_t)qorvoPlatGotoSleepCheck);
    platformUartInit();
    qorvoAlarmInit();
    qorvoRandomInit();
    qorvoRadioInit();
    gprcpTimedInit(); /* hassmic (gprcp): the chip's clock for timed transmit, once the driver is open (timed.c) */
}

bool otSysPseudoResetWasRequested(void)
{
    return false;
}

void otSysProcessDrivers(otInstance *aInstance)
{
    if (localInstance == NULL)
    {
        // local copy in case we need to perform a callback.
        localInstance = aInstance;
    }

    qorvoPlatMainLoop(!otTaskletsArePending(aInstance));
    platformUartProcess();
    // qorvoRadioProcess();
    // qorvoAlarmProcess();
}
