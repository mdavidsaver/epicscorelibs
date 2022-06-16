/*************************************************************************\
* Copyright (c) 2015 UChicago Argonne LLC, as Operator of Argonne
*     National Laboratory.
* Copyright (c) 2002 The Regents of the University of California, as
*     Operator of Los Alamos National Laboratory.
* SPDX-License-Identifier: EPICS
* EPICS BASE is distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution.
\*************************************************************************/

#define _VSB_CONFIG_FILE <../lib/h/config/vsbConfig.h>

#include <vxWorks.h>
#include <sntpcLib.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <taskLib.h>

#define EPICS_EXPOSE_LIBCOM_MONOTONIC_PRIVATE
#include "epicsTime.h"
#include "osiNTPTime.h"
#include "osiClockTime.h"
#include "generalTimeSup.h"
#include "envDefs.h"

#define NTP_REQUEST_TIMEOUT 4 /* seconds */

extern "C" {
    int tz2timezone(void);
}

static char sntp_sync_task[] = "ipsntps";
static char ntp_daemon[] = "ipntpd";

static const char *pserverAddr = NULL;
static CLOCKTIME_SYNCHOOK prevHook;

extern char* sysBootLine;

static void timeSync(int synchronized) {
    if (prevHook)
        prevHook(synchronized);

    if (synchronized) {
        struct timespec tsNow;
        if (clock_gettime(CLOCK_REALTIME, &tsNow) != OK)
            return;

        struct tm tmNow;
        localtime_r(&tsNow.tv_sec, &tmNow);

        static int lastYear = 0;
        if (tmNow.tm_year > lastYear && !tz2timezone())
            lastYear = tmNow.tm_year;
    }
}

static int timeRegister(void)
{
    /* If TZ not defined, set it from EPICS_TZ */
    if (getenv("TZ") == NULL) {
        const char *tz = envGetConfigParamPtr(&EPICS_TZ);

        if (tz && *tz) {
            epicsEnvSet("TZ", tz);

            /* Call tz2timezone() from the sync thread, needs the year */
            prevHook = ClockTime_syncHook;
            ClockTime_syncHook = timeSync;
        }
        else if (getenv("TIMEZONE") == NULL)
            printf("timeRegister: No Time Zone Information available\n");
    }

    // Define EPICS_TS_FORCE_NTPTIME to force use of NTPTime provider
    bool useNTP = getenv("EPICS_TS_FORCE_NTPTIME") != NULL;

    if (!useNTP) {
        int tid;
        if ((tid = taskNameToId(sntp_sync_task)) != ERROR ||
            (tid = taskNameToId(ntp_daemon)) != ERROR) {
            // A VxWorks 6 SNTP/NTP sync task exists
            struct timespec clockNow;

            useNTP = taskIsSuspended(tid) ||
                clock_gettime(CLOCK_REALTIME, &clockNow) != OK ||
                clockNow.tv_sec < BUILD_TIME;

            if (!useNTP) // Clock should be sync'd so we can run this:
                tz2timezone();
        }
        else
            useNTP = 1;
    }

    if (useNTP) {
        // Start NTP first so it can be used to sync ClockTime
        NTPTime_Init(100);
        ClockTime_Init(CLOCKTIME_SYNC);
    } else {
        ClockTime_Init(CLOCKTIME_NOSYNC);
    }
    osdMonotonicInit();
    return 1;
}
static int done = timeRegister();


// Routines for NTPTime provider

int osdNTPGet(struct timespec *ts)
{
    return sntpcTimeGet(const_cast<char *>(pserverAddr),
        NTP_REQUEST_TIMEOUT * sysClkRateGet(), ts);
}

void osdNTPInit(void)
{
    pserverAddr = envGetConfigParamPtr(&EPICS_TS_NTP_INET);
    if (!pserverAddr) { /* use the boot host */
        BOOT_PARAMS bootParms;
        static char host_addr[BOOT_ADDR_LEN];

        bootStringToStruct(sysBootLine, &bootParms);
        /* bootParms.had = host IP address */
        strncpy(host_addr, bootParms.had, BOOT_ADDR_LEN);
        pserverAddr = host_addr;
    }
}

void osdNTPReport(void)
{
    if (pserverAddr)
        printf("NTP Server = %s\n", pserverAddr);
}

void osdClockReport(int level)
{
    const char * ntpTask;
    int tid;

    if ((tid = taskNameToId(sntp_sync_task)) != ERROR)
        ntpTask = sntp_sync_task;
    else if ((tid = taskNameToId(ntp_daemon)) != ERROR)
        ntpTask = ntp_daemon;
    else {
        printf("No VxWorks OS clock sync tasks exist\n");
        return;
    }
    printf("VxWorks OS clock sync task '%s' is %s\n", ntpTask,
        taskIsSuspended(tid) ? "suspended" : "running");
}

// Other Time Routines

// vxWorks localtime_r returns different things in different versions.
// It can't fail though, so we just ignore the return value.
int epicsTime_localtime(const time_t *clock, struct tm *result)
{
    localtime_r(clock, result);
    return epicsTimeOK;
}

// vxWorks gmtime_r returns different things in different versions.
// It can't fail though, so we just ignore the return value.
int epicsTime_gmtime ( const time_t *pAnsiTime, struct tm *pTM )
{
    gmtime_r(pAnsiTime, pTM);
    return epicsTimeOK;
}
