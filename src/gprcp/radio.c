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
 *   This file implements the OpenThread platform abstraction for radio communication.
 *
 */

#include <string.h>
#include <time.h>

#include <openthread/platform/diag.h>
#include <openthread/platform/radio.h>
#include <openthread/platform/time.h>
#include <stdio.h>
#include <stdlib.h>

#include "utils/code_utils.h"

#include "radio_qorvo.h"

enum
{
    QPG_RECEIVE_SENSITIVITY = -100, // [dBm]
};

enum
{
    IEEE802154_ACK_LENGTH     = 5,
    IEEE802154_FRAME_TYPE_ACK = 0x2,
    IEEE802154_FRAME_PENDING  = 1 << 4,
    IEEE802154_DSN_OFFSET     = 2,
};

extern otRadioFrame sTransmitFrame;

static otRadioState sState;

/* hassmic (gprcp): timed.c */
otRadioCaps gprcpTimedCaps(otRadioCaps aCaps);
void        gprcpTimedArm(const otRadioFrame *aFrame);
void        gprcpTimedDone(const otRadioFrame *aFrame, otError aError);
static int  ack_log(void);   /* below cbQorvoRadioReceiveDone */
static otInstance  *pQorvoInstance;

typedef struct otCachedSettings_s
{
    uint16_t panid;
} otCachedSettings_t;

static otCachedSettings_t otCachedSettings;

/* Upper layer relies on txpower could be set before receive, but MAC have per-channel config for it.
   Store txpower until channel set in Receive(). */
#define PENDING_TX_POWER_NONE (-1)
static int8_t pendingTxPower = PENDING_TX_POWER_NONE;

static uint8_t sScanstate         = 0;
static int8_t  sLastReceivedPower = 127;

/* hassmic (gprcp): what the chip and its driver do themselves, as Qorvo's library says (0x1f: ack timeout, energy scan,
 * retries, CSMA backoff, sleep to transmit: what Amazon's ot-rcp reports), and timed transmit on the chip's clock where
 * hassmic_gptime is loaded (timed.c).  Transmit security, and timed transmit without the module, are OpenThread's, in
 * software (OPENTHREAD_CONFIG_MAC_SOFTWARE_TX_SECURITY_ENABLE, _TX_TIMING_ENABLE: SubMac adds what we do not report) */
otRadioCaps otPlatRadioGetCaps(otInstance *aInstance)
{
    OT_UNUSED_VARIABLE(aInstance);

    return gprcpTimedCaps(qorvoRadioGetCaps());
}

const char *otPlatRadioGetVersionString(otInstance *aInstance)
{
    OT_UNUSED_VARIABLE(aInstance);

    return "OPENTHREAD/hassmic-gprcp ;GP712";
}

int8_t otPlatRadioGetReceiveSensitivity(otInstance *aInstance)
{
    OT_UNUSED_VARIABLE(aInstance);

    return QPG_RECEIVE_SENSITIVITY;
}

void otPlatRadioGetIeeeEui64(otInstance *aInstance, uint8_t *aIeeeEui64)
{
    OT_UNUSED_VARIABLE(aInstance);

    qorvoRadioGetIeeeEui64(aIeeeEui64);
}

void otPlatRadioSetPanId(otInstance *aInstance, otPanId aPanId)
{
    OT_UNUSED_VARIABLE(aInstance);

    qorvoRadioSetPanId((uint16_t)aPanId);
    otCachedSettings.panid = aPanId;
}

void otPlatRadioSetExtendedAddress(otInstance *aInstance, const otExtAddress *address)
{
    OT_UNUSED_VARIABLE(aInstance);

    qorvoRadioSetExtendedAddress(address->m8);
}

void otPlatRadioSetShortAddress(otInstance *aInstance, otShortAddress aShortAddress)
{
    OT_UNUSED_VARIABLE(aInstance);

    qorvoRadioSetShortAddress((uint16_t)aShortAddress);
}

otError otPlatRadioGetTransmitPower(otInstance *aInstance, int8_t *aPower)
{
    OT_UNUSED_VARIABLE(aInstance);

    otError result;

    if (aPower == NULL)
    {
        return OT_ERROR_INVALID_ARGS;
    }

    if ((sState == OT_RADIO_STATE_DISABLED) || (sScanstate != 0))
    {
        *aPower = (pendingTxPower == PENDING_TX_POWER_NONE) ? 0 : pendingTxPower;
        return OT_ERROR_NONE;
    }

    result = qorvoRadioGetTransmitPower(aPower);

    if (result == OT_ERROR_INVALID_STATE)
    {
        // Channel was not set, so txpower is ambigious
        *aPower = (pendingTxPower == PENDING_TX_POWER_NONE) ? 0 : pendingTxPower;
        return OT_ERROR_NONE;
    }

    return result;
}

otError otPlatRadioSetTransmitPower(otInstance *aInstance, int8_t aPower)
{
    OT_UNUSED_VARIABLE(aInstance);

    otError result;

    if ((sState == OT_RADIO_STATE_DISABLED) || (sScanstate != 0))
    {
        pendingTxPower = aPower;
        return OT_ERROR_NONE;
    }

    result = qorvoRadioSetTransmitPower(aPower);

    if (result == OT_ERROR_INVALID_STATE)
    {
        // Channel was not set, so txpower is ambigious
        pendingTxPower = aPower;
        result         = OT_ERROR_NONE;
    }

    return result;
}

otError otPlatRadioGetCcaEnergyDetectThreshold(otInstance *aInstance, int8_t *aThreshold)
{
    OT_UNUSED_VARIABLE(aInstance);
    OT_UNUSED_VARIABLE(aThreshold);

    return OT_ERROR_NOT_IMPLEMENTED;
}

otError otPlatRadioSetCcaEnergyDetectThreshold(otInstance *aInstance, int8_t aThreshold)
{
    OT_UNUSED_VARIABLE(aInstance);
    OT_UNUSED_VARIABLE(aThreshold);

    return OT_ERROR_NOT_IMPLEMENTED;
}

otError otPlatRadioGetFemLnaGain(otInstance *aInstance, int8_t *aGain)
{
    OT_UNUSED_VARIABLE(aInstance);
    OT_UNUSED_VARIABLE(aGain);

    return OT_ERROR_NOT_IMPLEMENTED;
}

otError otPlatRadioSetFemLnaGain(otInstance *aInstance, int8_t aGain)
{
    OT_UNUSED_VARIABLE(aInstance);
    OT_UNUSED_VARIABLE(aGain);

    return OT_ERROR_NOT_IMPLEMENTED;
}

bool otPlatRadioGetPromiscuous(otInstance *aInstance)
{
    OT_UNUSED_VARIABLE(aInstance);

    return qorvoRadioGetPromiscuous();
}

void otPlatRadioSetPromiscuous(otInstance *aInstance, bool aEnable)
{
    OT_UNUSED_VARIABLE(aInstance);

    qorvoRadioSetPromiscuous(aEnable || ack_log());   /* hassmic (gprcp): GPRCP_ACK_LOG, tests only */
}

/* hassmic (gprcp): the clock of the driver's receive and transmit timestamps (getrawmonotonic, in us: docs/re-gp712.md).
 * Not qorvoRadioGetNow(): it asks GetCurrentTimeUs (command 157), which the Echo 4's driver (API 2.7) does not serve,
 * and Qorvo's library asserts on its refusal */
uint64_t otPlatTimeGet(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

uint32_t otPlatRadioGetBusSpeed(otInstance *aInstance)
{
    OT_UNUSED_VARIABLE(aInstance);

    return 0;
}

otError otPlatRadioEnable(otInstance *aInstance)
{
    pQorvoInstance = aInstance;
    memset(&otCachedSettings, 0x00, sizeof(otCachedSettings_t));

    if (!otPlatRadioIsEnabled(aInstance))
    {
        sState = OT_RADIO_STATE_SLEEP;
    }

    if (ack_log()) qorvoRadioSetPromiscuous(true);  /* hassmic (gprcp): GPRCP_ACK_LOG, tests only */
    return OT_ERROR_NONE;
}

otError otPlatRadioDisable(otInstance *aInstance)
{
    OT_UNUSED_VARIABLE(aInstance);

    otEXPECT(otPlatRadioIsEnabled(aInstance));

    if (sState == OT_RADIO_STATE_RECEIVE)
    {
        qorvoRadioSetRxOnWhenIdle(false);
    }

    sState = OT_RADIO_STATE_DISABLED;

exit:
    return OT_ERROR_NONE;
}

bool otPlatRadioIsEnabled(otInstance *aInstance)
{
    OT_UNUSED_VARIABLE(aInstance);

    return (sState != OT_RADIO_STATE_DISABLED);
}

otError otPlatRadioSleep(otInstance *aInstance)
{
    OT_UNUSED_VARIABLE(aInstance);

    otError error = OT_ERROR_INVALID_STATE;

    if (sState == OT_RADIO_STATE_RECEIVE || sState == OT_RADIO_STATE_SLEEP)
    {
        qorvoRadioSetRxOnWhenIdle(false);
        error  = OT_ERROR_NONE;
        sState = OT_RADIO_STATE_SLEEP;
    }
    return error;
}

otError otPlatRadioReceive(otInstance *aInstance, uint8_t aChannel)
{
    otError error = OT_ERROR_INVALID_STATE;

    pQorvoInstance = aInstance;

    if ((sState != OT_RADIO_STATE_DISABLED) && (sScanstate == 0))
    {
        qorvoRadioSetCurrentChannel(aChannel);
        if (pendingTxPower != PENDING_TX_POWER_NONE)
        {
            qorvoRadioSetTransmitPower(pendingTxPower);
            pendingTxPower = PENDING_TX_POWER_NONE;
        }
        error = OT_ERROR_NONE;
    }

    if (sState == OT_RADIO_STATE_SLEEP)
    {
        qorvoRadioSetRxOnWhenIdle(true);
        if (ack_log()) qorvoRadioSetPromiscuous(true);   /* hassmic (gprcp): tests only, as at Enable */
        error  = OT_ERROR_NONE;
        sState = OT_RADIO_STATE_RECEIVE;
    }

    return error;
}

/* hassmic (gprcp), tests only (tools/gp712/cslsweep.py): GPRCP_ACK_LOG=1 asks the chip to be promiscuous, whatever
 * the host asks, and says every ack frame it hands up on stderr (not handed on): the acks on the air.  The chip hands
 * up few of the acks its filter matched (2026-11-28): a lead, not a count.  Unset in the product. */
static int sAckLog = -1;

static int ack_log(void)
{
    if (sAckLog < 0) { const char *e = getenv("GPRCP_ACK_LOG"); sAckLog = e && *e == '1'; }
    return sAckLog;
}

void cbQorvoRadioReceiveDone(otRadioFrame *aFrame, otError aError)
{

    if (aError == OT_ERROR_NONE)
    {
        sLastReceivedPower = aFrame->mInfo.mRxInfo.mRssi;
    }

    if (ack_log() && aError == OT_ERROR_NONE && aFrame->mLength >= 3 && (aFrame->mPsdu[0] & 7) == 2)
    {
        char hex[2 * 40 + 1];
        int  n = aFrame->mLength < 40 ? aFrame->mLength : 40;
        for (int i = 0; i < n; i++) snprintf(hex + 2 * i, 3, "%02x", aFrame->mPsdu[i]);
        fprintf(stderr, "gprcp: ack heard seq %u, %u bytes, %d dBm, at %llu: %s\n", aFrame->mPsdu[2], aFrame->mLength,
                aFrame->mInfo.mRxInfo.mRssi, (unsigned long long)aFrame->mInfo.mRxInfo.mTimestamp, hex);
        return;
    }

    otPlatRadioReceiveDone(pQorvoInstance, aFrame, aError);
}

otRadioFrame *otPlatRadioGetTransmitBuffer(otInstance *aInstance)
{
    OT_UNUSED_VARIABLE(aInstance);

    return &sTransmitFrame;
}

otError otPlatRadioTransmit(otInstance *aInstance, otRadioFrame *aFrame)
{
    otError err = OT_ERROR_NONE;

    pQorvoInstance = aInstance;

    otEXPECT_ACTION(sState != OT_RADIO_STATE_DISABLED, err = OT_ERROR_INVALID_STATE);

    gprcpTimedArm(aFrame);      /* hassmic (gprcp): a frame with a target time staged for it (timed.c) */
    err = qorvoRadioTransmit(aFrame);

exit:
    return err;
}

void cbQorvoRadioTransmitDone(otRadioFrame *aFrame, bool aFramePending, otError aError)
{
    // TODO: pass received ACK frame instead of generating one.
    otRadioFrame ackFrame;
    uint8_t      psdu[IEEE802154_ACK_LENGTH];

    ackFrame.mPsdu    = psdu;
    ackFrame.mLength  = IEEE802154_ACK_LENGTH;
    ackFrame.mPsdu[0] = IEEE802154_FRAME_TYPE_ACK;

    if (aFramePending)
    {
        ackFrame.mPsdu[0] |= IEEE802154_FRAME_PENDING;
    }

    ackFrame.mPsdu[1] = 0;
    ackFrame.mPsdu[2] = aFrame->mPsdu[IEEE802154_DSN_OFFSET];

    gprcpTimedDone(aFrame, aError);     /* hassmic (gprcp): a line for a timed frame where a test asks (timed.c) */
    otPlatRadioTxDone(pQorvoInstance, aFrame, (aError == OT_ERROR_NONE) ? &ackFrame : NULL, aError);
}

int8_t otPlatRadioGetRssi(otInstance *aInstance)
{
    OT_UNUSED_VARIABLE(aInstance);

    return sLastReceivedPower;
}

otError otPlatRadioEnergyScan(otInstance *aInstance, uint8_t aScanChannel, uint16_t aScanDuration)
{
    OT_UNUSED_VARIABLE(aInstance);

    sScanstate = 1;
    return qorvoRadioEnergyScan(aScanChannel, aScanDuration);
}

/* hassmic (gprcp): what Amazon's driver puts into the scan's result list is 802.15.4's ED level, 0..255, not dBm
 * (gpHal_cbEDConfirm through gpHal_CalculateED: 0 below -89 dBm, 255 from -38, floor((dBm + 89) * 5.1) between), and
 * Qorvo's library hands that byte on read as int8 (gpMacDispatcher_cbScanConfirm's ldrsb), as Amazon's ot-rcp does:
 * +122, -114 seen were -65, -61 dBm.  Turned back here to the dBm it came from (docs/re-gp712.md "Energy scan") */
void cbQorvoRadioEnergyScanDone(int8_t aEnergyScanMaxRssi)
{
    uint8_t ed = (uint8_t)aEnergyScanMaxRssi;

    sScanstate = 0;
    otPlatRadioEnergyScanDone(pQorvoInstance, (int8_t)(-89 + (ed * 10 + 50) / 51));
}

void otPlatRadioEnableSrcMatch(otInstance *aInstance, bool aEnable)
{
    OT_UNUSED_VARIABLE(aInstance);

    qorvoRadioEnableSrcMatch(aEnable);
}

otError otPlatRadioAddSrcMatchShortEntry(otInstance *aInstance, otShortAddress aShortAddress)
{
    OT_UNUSED_VARIABLE(aInstance);

    return qorvoRadioAddSrcMatchShortEntry((uint16_t)aShortAddress, otCachedSettings.panid);
}

otError otPlatRadioAddSrcMatchExtEntry(otInstance *aInstance, const otExtAddress *aExtAddress)
{
    OT_UNUSED_VARIABLE(aInstance);

    return qorvoRadioAddSrcMatchExtEntry(aExtAddress->m8, otCachedSettings.panid);
}

otError otPlatRadioClearSrcMatchShortEntry(otInstance *aInstance, otShortAddress aShortAddress)
{
    OT_UNUSED_VARIABLE(aInstance);

    return qorvoRadioClearSrcMatchShortEntry((uint16_t)aShortAddress, otCachedSettings.panid);
}

otError otPlatRadioClearSrcMatchExtEntry(otInstance *aInstance, const otExtAddress *aExtAddress)
{
    OT_UNUSED_VARIABLE(aInstance);

    return qorvoRadioClearSrcMatchExtEntry(aExtAddress->m8, otCachedSettings.panid);
}

void otPlatRadioClearSrcMatchShortEntries(otInstance *aInstance)
{
    OT_UNUSED_VARIABLE(aInstance);

    /* clear both short and extended addresses here */
    qorvoRadioClearSrcMatchEntries();
}

void otPlatRadioClearSrcMatchExtEntries(otInstance *aInstance)
{
    OT_UNUSED_VARIABLE(aInstance);

    /* not implemented */
    /* assumes clearing of short and extended entries is done simultaniously by the openthread stack */
}
uint32_t otPlatRadioGetSupportedChannelMask(otInstance *aInstance)
{
    OT_UNUSED_VARIABLE(aInstance);

    return 0x03fff800;
}

uint32_t otPlatRadioGetPreferredChannelMask(otInstance *aInstance)
{
    OT_UNUSED_VARIABLE(aInstance);

    return 0x03fff800;
}

otError otPlatRadioSetCoexEnabled(otInstance *aInstance, bool aEnabled)
{
    OT_UNUSED_VARIABLE(aInstance);
    OT_UNUSED_VARIABLE(aEnabled);

    return OT_ERROR_NOT_IMPLEMENTED;
}

bool otPlatRadioIsCoexEnabled(otInstance *aInstance)
{
    OT_UNUSED_VARIABLE(aInstance);

    return false;
}

otError otPlatRadioGetCoexMetrics(otInstance *aInstance, otRadioCoexMetrics *aCoexMetrics)
{
    OT_UNUSED_VARIABLE(aInstance);
    OT_UNUSED_VARIABLE(aCoexMetrics);

    return OT_ERROR_NOT_IMPLEMENTED;
}

otError otPlatRadioSetChannelMaxTransmitPower(otInstance *aInstance, uint8_t aChannel, int8_t aMaxPower)
{
    OT_UNUSED_VARIABLE(aInstance);
    OT_UNUSED_VARIABLE(aChannel);
    OT_UNUSED_VARIABLE(aMaxPower);

    return OT_ERROR_NOT_IMPLEMENTED;
}

otError otPlatRadioSetRegion(otInstance *aInstance, uint16_t aRegionCode)
{
    OT_UNUSED_VARIABLE(aInstance);
    OT_UNUSED_VARIABLE(aRegionCode);

    return OT_ERROR_NOT_IMPLEMENTED;
}

otError otPlatRadioGetRegion(otInstance *aInstance, uint16_t *aRegionCode)
{
    OT_UNUSED_VARIABLE(aInstance);
    OT_UNUSED_VARIABLE(aRegionCode);

    return OT_ERROR_NOT_IMPLEMENTED;
}

#if QORVO_FTD
#include <openthread/thread_ftd.h>

void qorvoNeighbourTableChanged(otNeighborTableEvent aEvent, const otNeighborTableEntryInfo *aEntryInfo)
{
    if (aEvent == OT_NEIGHBOR_TABLE_EVENT_CHILD_REMOVED)
    {
        qorvoRadioHandleChildRemoved(aEntryInfo->mInfo.mChild.mRloc16, aEntryInfo->mInfo.mChild.mExtAddress.m8);
    }
    else if (aEvent == OT_NEIGHBOR_TABLE_EVENT_CHILD_ADDED)
    {
        qorvoRadioHandleChildAdded(aEntryInfo->mInfo.mChild.mRloc16, aEntryInfo->mInfo.mChild.mExtAddress.m8);
    }
    // We don't care about the other events
}

#if QORVO_FTD
void qorvoRegisterNeighbourTableCallback(bool aEnable)
{
    otThreadRegisterNeighborTableCallback(pQorvoInstance, (aEnable) ? qorvoNeighbourTableChanged : NULL);
}
#else
extern void rcpRegisterNeighborTableCallback(bool aEnable);

void qorvoRegisterNeighbourTableCallback(bool aEnable)
{
    rcpRegisterNeighborTableCallback(aEnable);
}

OT_TOOL_WEAK void qorvoRadioHandleChildAdded(uint16_t aShortAddress, const uint8_t *aExtAddress)
{
}
OT_TOOL_WEAK void qorvoRadioHandleChildRemoved(uint16_t aShortAddress, const uint8_t *aExtAddress)
{
}
#endif // QORVO_FTD
#else
OT_TOOL_WEAK void qorvoRegisterNeighbourTableCallback(bool aEnable)
{
}
#endif // QORVO_RCP || QORVO_FTD
