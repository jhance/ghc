/* -----------------------------------------------------------------------------
 *
 * (c) The GHC Team, 1995-2007
 *
 * Interval timer for profiling and pre-emptive scheduling.
 *
 * ---------------------------------------------------------------------------*/

/*
 * The interval timer is used for profiling and for context switching in the
 * threaded build.  Though POSIX 1003.1b includes a standard interface for
 * such things, no one really seems to be implementing them yet.  Even 
 * Solaris 2.3 only seems to provide support for @CLOCK_REAL@, whereas we're
 * keen on getting access to @CLOCK_VIRTUAL@.
 * 
 * Hence, we use the old-fashioned @setitimer@ that just about everyone seems
 * to support.  So much for standards.
 */

#include "PosixSource.h"
#include "Rts.h"

#include "Ticker.h"
#include "Itimer.h"
#include "Proftimer.h"
#include "Schedule.h"
#include "Select.h"

/* As recommended in the autoconf manual */
# ifdef TIME_WITH_SYS_TIME
#  include <sys/time.h>
#  include <time.h>
# else
#  ifdef HAVE_SYS_TIME_H
#   include <sys/time.h>
#  else
#   include <time.h>
#  endif
# endif

#ifdef HAVE_SIGNAL_H
# include <signal.h>
#endif

#include <string.h>

/*
 * We use a realtime timer by default.  I found this much more
 * reliable than a CPU timer:
 *
 * Experiments with different frequences: using
 * CLOCK_REALTIME/CLOCK_MONOTONIC on Linux 2.6.32,
 *     1000us has  <1% impact on runtime
 *      100us has  ~2% impact on runtime
 *       10us has ~40% impact on runtime
 *
 * using CLOCK_PROCESS_CPUTIME_ID on Linux 2.6.32,
 *     I cannot get it to tick faster than 10ms (10000us)
 *     which isn't great for profiling.
 *
 * In the threaded RTS, we can't tick in CPU time because the thread
 * which has the virtual timer might be idle, so the tick would never
 * fire.  Therfore we used to tick in realtime in the threaded RTS and
 * in CPU time otherwise, but now we always tick in realtime, for
 * several reasons:
 *
 *   - resolution (see above)
 *   - consistency (-threaded is the same as normal)
 *   - more consistency: Windows only has a realtime timer
 *
 * Note we want to use CLOCK_MONOTONIC rather than CLOCK_REALTIME,
 * because the latter may jump around (NTP adjustments, leap seconds
 * etc.).
 */

#if defined(USE_TIMER_CREATE)
#  define ITIMER_SIGNAL SIGVTALRM
#elif defined(HAVE_SETITIMER)
#  define ITIMER_SIGNAL  SIGALRM
   // Using SIGALRM can leads to problems, see #850.  But we have no
   // option if timer_create() is not available.
#else
#  error No way to set an interval timer.
#endif

#if defined(USE_TIMER_CREATE)
static timer_t timer;
#endif

static Time itimer_interval = DEFAULT_TICK_INTERVAL;

static void install_vtalrm_handler(TickProc handle_tick)
{
    struct sigaction action;

    action.sa_handler = handle_tick;

    sigemptyset(&action.sa_mask);

#ifdef SA_RESTART
    // specify SA_RESTART.  One consequence if we don't do this is
    // that readline gets confused by the -threaded RTS.  It seems
    // that if a SIGALRM handler is installed without SA_RESTART,
    // readline installs its own SIGALRM signal handler (see
    // readline's signals.c), and this somehow causes readline to go
    // wrong when the input exceeds a single line (try it).
    action.sa_flags = SA_RESTART;
#else
    action.sa_flags = 0;
#endif

    if (sigaction(ITIMER_SIGNAL, &action, NULL) == -1) {
        sysErrorBelch("sigaction");
        stg_exit(EXIT_FAILURE);
    }
}

void
initTicker (Time interval, TickProc handle_tick)
{
    itimer_interval = interval;

#if defined(USE_TIMER_CREATE)
    {
        struct sigevent ev;
        clockid_t clock;

        // Keep programs like valgrind happy
        memset(&ev, 0, sizeof(ev));

        ev.sigev_notify = SIGEV_SIGNAL;
        ev.sigev_signo  = ITIMER_SIGNAL;

#if defined(CLOCK_MONOTONIC)
        clock = CLOCK_MONOTONIC;
#else
        clock = CLOCK_REALTIME;
#endif

        if (timer_create(clock, &ev, &timer) != 0) {
            sysErrorBelch("timer_create");
            stg_exit(EXIT_FAILURE);
        }
    }
#endif

    install_vtalrm_handler(handle_tick);
}

void
startTicker(void)
{
#if defined(USE_TIMER_CREATE)
    {
        struct itimerspec it;
        
        it.it_value.tv_sec  = TimeToSeconds(itimer_interval);
        it.it_value.tv_nsec = TimeToNS(itimer_interval) % 1000000000;
        it.it_interval = it.it_value;
        
        if (timer_settime(timer, 0, &it, NULL) != 0) {
            sysErrorBelch("timer_settime");
            stg_exit(EXIT_FAILURE);
        }
    }
#else
    {
        struct itimerval it;

        it.it_value.tv_sec = TimeToSeconds(itimer_interval);
        it.it_value.tv_usec = TimeToUS(itimer_interval) % 1000000;
        it.it_interval = it.it_value;
        
        if (setitimer(ITIMER_REAL, &it, NULL) != 0) {
            sysErrorBelch("setitimer");
            stg_exit(EXIT_FAILURE);
        }
    }
#endif
}

void
stopTicker(void)
{
#if defined(USE_TIMER_CREATE)
    struct itimerspec it;

    it.it_value.tv_sec = 0;
    it.it_value.tv_nsec = 0;
    it.it_interval = it.it_value;

    if (timer_settime(timer, 0, &it, NULL) != 0) {
        sysErrorBelch("timer_settime");
        stg_exit(EXIT_FAILURE);
    }
#else
    struct itimerval it;

    it.it_value.tv_sec = 0;
    it.it_value.tv_usec = 0;
    it.it_interval = it.it_value;

    if (setitimer(ITIMER_REAL, &it, NULL) != 0) {
        sysErrorBelch("setitimer");
        stg_exit(EXIT_FAILURE);
    }
#endif
}

void
exitTicker (rtsBool wait STG_UNUSED)
{
#if defined(USE_TIMER_CREATE)
    timer_delete(timer);
    // ignore errors - we don't really care if it fails.
#endif
}

int
rtsTimerSignal(void)
{
    return ITIMER_SIGNAL;
}
