/* -*- tab-width: 8; c-basic-offset: 4 -*- */

/*
 * MMSYTEM time functions
 *
 * Copyright 1993 Martin Ayotte
 */

#include <time.h>
#include <sys/time.h>
#include "winbase.h"
#include "callback.h"
#include "multimedia.h"
#include "services.h"
#include "syslevel.h"
#include "debugtools.h"

DEFAULT_DEBUG_CHANNEL(mmtime)

/*
 * FIXME
 * We're using "1" as the mininum resolution to the timer,
 * as Windows 95 does, according to the docs. Maybe it should
 * depend on the computers resources!
 */
#define MMSYSTIME_MININTERVAL /* (1) */ (10)
#define MMSYSTIME_MAXINTERVAL (65535)

static	void	TIME_TriggerCallBack(LPWINE_TIMERENTRY lpTimer, DWORD dwCurrent)
{
    TRACE("before CallBack (%lu)!\n", dwCurrent);
    TRACE("lpFunc=%p wTimerID=%04X dwUser=%08lX !\n",
	  lpTimer->lpFunc, lpTimer->wTimerID, lpTimer->dwUser);
	
    /* - TimeProc callback that is called here is something strange, under Windows 3.1x it is called 
     * 		during interrupt time,  is allowed to execute very limited number of API calls (like
     *	    	PostMessage), and must reside in DLL (therefore uses stack of active application). So I 
     *       guess current implementation via SetTimer has to be improved upon.		
     */
    switch (lpTimer->wFlags & 0x30) {
    case TIME_CALLBACK_FUNCTION:
	if (lpTimer->wFlags & WINE_TIMER_IS32)
	    ((LPTIMECALLBACK)lpTimer->lpFunc)(lpTimer->wTimerID, 0, lpTimer->dwUser, 0, 0);
	else
	    Callbacks->CallTimeFuncProc(lpTimer->lpFunc,
					lpTimer->wTimerID, 0,
					lpTimer->dwUser, 0, 0);
	break;
    case TIME_CALLBACK_EVENT_SET:
	SetEvent((HANDLE)lpTimer->lpFunc);
	break;
    case TIME_CALLBACK_EVENT_PULSE:
	PulseEvent((HANDLE)lpTimer->lpFunc);
	break;
    default:
	FIXME("Unknown callback type 0x%04x for mmtime callback (%p), ignored.\n", lpTimer->wFlags, lpTimer->lpFunc);
	break;
    }
    TRACE("after CallBack !\n");
}

/**************************************************************************
 *           TIME_MMSysTimeCallback
 */
static void CALLBACK TIME_MMSysTimeCallback(ULONG_PTR ptr_)
{
    LPWINE_TIMERENTRY 	lpTimer, lpNextTimer;
    LPWINE_MM_IDATA	iData = (LPWINE_MM_IDATA)ptr_;
    int			idx;

    iData->mmSysTimeMS += MMSYSTIME_MININTERVAL;

    /* since timeSetEvent() and timeKillEvent() can be called
     * from 16 bit code, there are cases where win16 lock is
     * locked upon entering timeSetEvent(), and then the mm timer 
     * critical section is locked. This function cannot call the
     * timer callback with the crit sect locked (because callback
     * may need to acquire Win16 lock, thus providing a deadlock
     * situation).
     * To cope with that, we just copy the WINE_TIMERENTRY struct
     * that need to trigger the callback, and call it without the
     * mm timer crit sect locked. The bad side of this 
     * implementation is that, in some cases, the callback may be
     * invoked *after* a timer has been destroyed...
     * EPP 99/07/13
     */
    idx = 0;

    EnterCriticalSection(&iData->cs);
    for (lpTimer = iData->lpTimerList; lpTimer != NULL; ) {
	lpNextTimer = lpTimer->lpNext;
	if (lpTimer->wCurTime < MMSYSTIME_MININTERVAL) {
	    /* since lpTimer->wDelay is >= MININTERVAL, wCurTime value
	     * shall be correct (>= 0)
	     */
	    lpTimer->wCurTime += lpTimer->wDelay - MMSYSTIME_MININTERVAL;
	    if (lpTimer->lpFunc) {
		if (idx == iData->nSizeLpTimers) {
		    iData->lpTimers = (LPWINE_TIMERENTRY)
			HeapReAlloc(GetProcessHeap(), 0, 
				    iData->lpTimers, 
				    ++iData->nSizeLpTimers * sizeof(WINE_TIMERENTRY));
		}
		iData->lpTimers[idx++] = *lpTimer;
	    }
	    if (lpTimer->wFlags & TIME_ONESHOT)
		timeKillEvent(lpTimer->wTimerID);
	} else {
	    lpTimer->wCurTime -= MMSYSTIME_MININTERVAL;
	}
	lpTimer = lpNextTimer;
    }
    LeaveCriticalSection(&iData->cs);

    while (idx > 0) {
	TIME_TriggerCallBack(iData->lpTimers + --idx, iData->mmSysTimeMS);
    }
}

/**************************************************************************
 * 				MULTIMEDIA_MMTimeStart		[internal]
 */
static	LPWINE_MM_IDATA	MULTIMEDIA_MMTimeStart(void)
{
    LPWINE_MM_IDATA	iData = MULTIMEDIA_GetIData();

    if (!iData || IsBadWritePtr(iData, sizeof(WINE_MM_IDATA))) {
	ERR("iData is not correctly set, please report. Expect failure.\n");
	return 0;
    }
    /* FIXME: the service timer is never killed, even when process is
     * detached from this DLL
     */
    /* one could think it's possible to stop the service thread activity when no more
     * mm timers are active, but this would require to keep mmSysTimeMS up-to-date
     * without being incremented within the service thread callback.
     */
    if (!iData->hMMTimer) {
	iData->mmSysTimeMS = GetTickCount();
	iData->lpTimerList = NULL;
	iData->hMMTimer = SERVICE_AddTimer(MMSYSTIME_MININTERVAL*1000L, TIME_MMSysTimeCallback, (DWORD)iData);
	InitializeCriticalSection(&iData->cs);
    }

    return iData;
}

/**************************************************************************
 * 				timeGetSystemTime	[WINMM.140]
 */
MMRESULT WINAPI timeGetSystemTime(LPMMTIME lpTime, UINT wSize)
{
    LPWINE_MM_IDATA	iData;

    TRACE("(%p, %u);\n", lpTime, wSize);

    if (wSize >= sizeof(*lpTime)) {
	iData = MULTIMEDIA_MMTimeStart();
	if (!iData)
	    return MMSYSERR_NOMEM;
	lpTime->wType = TIME_MS;
	lpTime->u.ms = iData->mmSysTimeMS;

	TRACE("=> %lu\n", iData->mmSysTimeMS);
    }

    return 0;
}

/**************************************************************************
 * 				timeGetSystemTime	[MMSYSTEM.601]
 */
MMRESULT16 WINAPI timeGetSystemTime16(LPMMTIME16 lpTime, UINT16 wSize)
{
    LPWINE_MM_IDATA	iData;

    TRACE("(%p, %u);\n", lpTime, wSize);

    if (wSize >= sizeof(*lpTime)) {
	iData = MULTIMEDIA_MMTimeStart();
	if (!iData)
	    return MMSYSERR_NOMEM;
	lpTime->wType = TIME_MS;
	lpTime->u.ms = iData->mmSysTimeMS;

	TRACE("=> %lu\n", iData->mmSysTimeMS);
    }

    return 0;
}

/**************************************************************************
 * 				timeSetEventInternal	[internal]
 */
static	WORD	timeSetEventInternal(UINT wDelay, UINT wResol,
				     FARPROC16 lpFunc, DWORD dwUser, UINT wFlags)
{
    WORD 		wNewID = 0;
    LPWINE_TIMERENTRY	lpNewTimer;
    LPWINE_TIMERENTRY	lpTimer;
    LPWINE_MM_IDATA	iData;

    TRACE("(%u, %u, %p, %08lX, %04X);\n", wDelay, wResol, lpFunc, dwUser, wFlags);

    lpNewTimer = (LPWINE_TIMERENTRY)HeapAlloc(GetProcessHeap(), 0, sizeof(WINE_TIMERENTRY));
    if (lpNewTimer == NULL)
	return 0;

    if (wDelay < MMSYSTIME_MININTERVAL || wDelay > MMSYSTIME_MAXINTERVAL)
	return 0;

    iData = MULTIMEDIA_MMTimeStart();

    if (!iData)
	return 0;

    lpNewTimer->wCurTime = wDelay;
    lpNewTimer->wDelay = wDelay;
    lpNewTimer->wResol = wResol;
    lpNewTimer->lpFunc = lpFunc;
    lpNewTimer->dwUser = dwUser;
    lpNewTimer->wFlags = wFlags;

    TRACE("lpFunc=0x%08lx !\n", (DWORD)lpFunc);

    EnterCriticalSection(&iData->cs);

    for (lpTimer = iData->lpTimerList; lpTimer != NULL; lpTimer = lpTimer->lpNext) {
	wNewID = MAX(wNewID, lpTimer->wTimerID);
    }
    
    lpNewTimer->lpNext = iData->lpTimerList;
    iData->lpTimerList = lpNewTimer;
    lpNewTimer->wTimerID = wNewID + 1;

    LeaveCriticalSection(&iData->cs);

    return wNewID + 1;
}

/**************************************************************************
 * 				timeSetEvent		[MMSYSTEM.602]
 */
MMRESULT WINAPI timeSetEvent(UINT wDelay, UINT wResol, LPTIMECALLBACK lpFunc,
			     DWORD dwUser, UINT wFlags)
{
    if (wFlags & WINE_TIMER_IS32)
	WARN("Unknown windows flag... wine internally used.. ooch\n");

    return timeSetEventInternal(wDelay, wResol, (FARPROC16)lpFunc, 
				dwUser, wFlags|WINE_TIMER_IS32);
}

/**************************************************************************
 * 				timeSetEvent		[MMSYSTEM.602]
 */
MMRESULT16 WINAPI timeSetEvent16(UINT16 wDelay, UINT16 wResol, LPTIMECALLBACK16 lpFunc, 
				 DWORD dwUser, UINT16 wFlags)
{
    if (wFlags & WINE_TIMER_IS32)
	WARN("Unknown windows flag... wine internally used.. ooch\n");

    return timeSetEventInternal(wDelay, wResol, (FARPROC16)lpFunc, 
				dwUser, wFlags & ~WINE_TIMER_IS32);
}

/**************************************************************************
 * 				timeKillEvent		[WINMM.142]
 */
MMRESULT WINAPI timeKillEvent(UINT wID)
{
    LPWINE_TIMERENTRY*	lpTimer;
    LPWINE_MM_IDATA	iData = MULTIMEDIA_GetIData();
    MMRESULT		ret = MMSYSERR_INVALPARAM;

    EnterCriticalSection(&iData->cs);
    /* remove WINE_TIMERENTRY from list */
    for (lpTimer = &iData->lpTimerList; *lpTimer; lpTimer = &((*lpTimer)->lpNext)) {
	if (wID == (*lpTimer)->wTimerID) {
	    *lpTimer = (*lpTimer)->lpNext;
	    break;
	}
    }
    LeaveCriticalSection(&iData->cs);
    
    if (*lpTimer) {
	HeapFree(GetProcessHeap(), 0, *lpTimer);
	ret = TIMERR_NOERROR;
    }

    return ret;
}

/**************************************************************************
 * 				timeKillEvent		[MMSYSTEM.603]
 */
MMRESULT16 WINAPI timeKillEvent16(UINT16 wID)
{
    return timeKillEvent(wID);
}

/**************************************************************************
 * 				timeGetDevCaps		[WINMM.139]
 */
MMRESULT WINAPI timeGetDevCaps(LPTIMECAPS lpCaps, UINT wSize)
{
    TRACE("(%p, %u) !\n", lpCaps, wSize);

    lpCaps->wPeriodMin = MMSYSTIME_MININTERVAL;
    lpCaps->wPeriodMax = MMSYSTIME_MAXINTERVAL;
    return 0;
}

/**************************************************************************
 * 				timeGetDevCaps		[MMSYSTEM.604]
 */
MMRESULT16 WINAPI timeGetDevCaps16(LPTIMECAPS16 lpCaps, UINT16 wSize)
{
    TRACE("(%p, %u) !\n", lpCaps, wSize);

    lpCaps->wPeriodMin = MMSYSTIME_MININTERVAL;
    lpCaps->wPeriodMax = MMSYSTIME_MAXINTERVAL;
    return 0;
}

/**************************************************************************
 * 				timeBeginPeriod		[WINMM.137]
 */
MMRESULT WINAPI timeBeginPeriod(UINT wPeriod)
{
    TRACE("(%u) !\n", wPeriod);

    if (wPeriod < MMSYSTIME_MININTERVAL || wPeriod > MMSYSTIME_MAXINTERVAL) 
	return TIMERR_NOCANDO;
    return 0;
}

/**************************************************************************
 * 				timeBeginPeriod16	[MMSYSTEM.605]
 */
MMRESULT16 WINAPI timeBeginPeriod16(UINT16 wPeriod)
{
    TRACE("(%u) !\n", wPeriod);

    if (wPeriod < MMSYSTIME_MININTERVAL || wPeriod > MMSYSTIME_MAXINTERVAL) 
	return TIMERR_NOCANDO;
    return 0;
}

/**************************************************************************
 * 				timeEndPeriod		[WINMM.138]
 */
MMRESULT WINAPI timeEndPeriod(UINT wPeriod)
{
    TRACE("(%u) !\n", wPeriod);

    if (wPeriod < MMSYSTIME_MININTERVAL || wPeriod > MMSYSTIME_MAXINTERVAL) 
	return TIMERR_NOCANDO;
    return 0;
}

/**************************************************************************
 * 				timeEndPeriod16		[MMSYSTEM.606]
 */
MMRESULT16 WINAPI timeEndPeriod16(UINT16 wPeriod)
{
    TRACE("(%u) !\n", wPeriod);

    if (wPeriod < MMSYSTIME_MININTERVAL || wPeriod > MMSYSTIME_MAXINTERVAL) 
	return TIMERR_NOCANDO;
    return 0;
}

/**************************************************************************
 * 				timeGetTime    [MMSYSTEM.607][WINMM.141]
 */
DWORD WINAPI timeGetTime(void)
{
    return MULTIMEDIA_MMTimeStart()->mmSysTimeMS;
}
