/*
 * Implementation of the Global Interpreter Lock (GIL).
 */

#include <stdlib.h>
#include <errno.h>


/* First some general settings */

/* microseconds (the Python API uses seconds, though) */
#define DEFAULT_INTERVAL 5000
static unsigned long gil_interval = DEFAULT_INTERVAL;
#define INTERVAL (gil_interval >= 1 ? gil_interval : 1)

/* Enable if you want to force the switching of threads at least every `gil_interval` */
#undef FORCE_SWITCHING
#define FORCE_SWITCHING


/*
   Notes about the implementation:
   - The GIL is just a boolean variable (gil_locked) whose access is protected
     by a mutex (gil_mutex), and whose changes are signalled by a condition
     variable (gil_cond). gil_mutex is taken for short periods of time,
     and therefore mostly uncontended.
   - In the GIL-holding thread, the main loop (PyEval_EvalFrameEx) must be
     able to release the GIL on demand by another thread. A volatile boolean
     variable (gil_drop_request) is used for that purpose, which is checked
     at every turn of the eval loop. That variable is set after a wait of
     `interval` microseconds on `gil_cond` has timed out.
      
      [Actually, another volatile boolean variable (eval_breaker) is used
       which ORs several conditions into one. Volatile booleans are
       sufficient as inter-thread signalling means since Python is run
       on cache-coherent architectures only.]
   - A thread wanting to take the GIL will first let pass a given amount of
     time (`interval` microseconds) before setting gil_drop_request. This
     encourages a defined switching period, but doesn't enforce it since
     opcodes can take an arbitrary time to execute.
 
     The `interval` value is available for the user to read and modify
     using the Python API `sys.{get,set}switchinterval()`.
   - When a thread releases the GIL and gil_drop_request is set, that thread
     ensures that another GIL-awaiting thread gets scheduled.
     It does so by waiting on a condition variable (switch_cond) until
     the value of gil_last_holder is changed to something else than its
     own thread state pointer, indicating that another thread was able to
     take the GIL.
 
     This is meant to prohibit the latency-adverse behaviour on multi-core
     machines where one thread would speculatively release the GIL, but still
     run and end up being the first to re-acquire it, making the "timeslices"
     much longer than expected.
     (Note: this mechanism is enabled with FORCE_SWITCHING above)
*/

#ifndef _POSIX_THREADS
/* This means pthreads are not implemented in libc headers, hence the macro
   not present in unistd.h. But they still can be implemented as an external
   library (e.g. gnu pth in pthread emulation) */
# ifdef HAVE_PTHREAD_H
#  include <pthread.h> /* _POSIX_THREADS */
# endif
#endif


#ifdef _POSIX_THREADS

/*
 * POSIX support
 */

#include <pthread.h>

#define ADD_MICROSECONDS(tv, interval) \
do { \
    tv.tv_usec += (long) interval; \
    tv.tv_sec += tv.tv_usec / 1000000; \
    tv.tv_usec %= 1000000; \
} while (0)

/* We assume all modern POSIX systems have gettimeofday() */
#ifdef GETTIMEOFDAY_NO_TZ
#define GETTIMEOFDAY(ptv) gettimeofday(ptv)
#else
#define GETTIMEOFDAY(ptv) gettimeofday(ptv, (struct timezone *)NULL)
#endif

#define MUTEX_T pthread_mutex_t
#define MUTEX_INIT(mut) \
    if (pthread_mutex_init(&mut, NULL)) { \
        Py_FatalError("pthread_mutex_init(" #mut ") failed"); };
#define MUTEX_LOCK(mut) \
    if (pthread_mutex_lock(&mut)) { \
        Py_FatalError("pthread_mutex_lock(" #mut ") failed"); };
#define MUTEX_UNLOCK(mut) \
    if (pthread_mutex_unlock(&mut)) { \
        Py_FatalError("pthread_mutex_unlock(" #mut ") failed"); };

#define COND_T pthread_cond_t
#define COND_INIT(cond) \
    if (pthread_cond_init(&cond, NULL)) { \
        Py_FatalError("pthread_cond_init(" #cond ") failed"); };
#define COND_PREPARE(cond)
#define COND_SIGNAL(cond) \
    if (pthread_cond_signal(&cond)) { \
        Py_FatalError("pthread_cond_signal(" #cond ") failed"); };
#define COND_WAIT(cond, mut) \
    if (pthread_cond_wait(&cond, &mut)) { \
        Py_FatalError("pthread_cond_wait(" #cond ") failed"); };
#define COND_TIMED_WAIT(cond, mut, microseconds, timeout_result) \
    { \
        int r; \
        struct timespec ts; \
        struct timeval deadline; \
        \
        GETTIMEOFDAY(&deadline); \
        ADD_MICROSECONDS(deadline, microseconds); \
        ts.tv_sec = deadline.tv_sec; \
        ts.tv_nsec = deadline.tv_usec * 1000; \
        \
        r = pthread_cond_timedwait(&cond, &mut, &ts); \
        if (r == ETIMEDOUT) \
            timeout_result = 1; \
        else if (r) \
            Py_FatalError("pthread_cond_timedwait(" #cond ") failed"); \
        else \
            timeout_result = 0; \
    } \

#elif defined(NT_THREADS)

/*
 * Windows (2000 and later, as well as (hopefully) CE) support
 */

#include <windows.h>

#define MUTEX_T HANDLE
#define MUTEX_INIT(mut) \
    if (!(mut = CreateMutex(NULL, FALSE, NULL))) { \
        Py_FatalError("CreateMutex(" #mut ") failed"); };
#define MUTEX_LOCK(mut) \
    if (WaitForSingleObject(mut, INFINITE) != WAIT_OBJECT_0) { \
        Py_FatalError("WaitForSingleObject(" #mut ") failed"); };
#define MUTEX_UNLOCK(mut) \
    if (!ReleaseMutex(mut)) { \
        Py_FatalError("ReleaseMutex(" #mut ") failed"); };

/* We emulate condition variables with events. It is sufficient here.
   (WaitForMultipleObjects() allows the event to be caught and the mutex
   to be taken atomically) */
#define COND_T HANDLE
#define COND_INIT(cond) \
    /* auto-reset, non-signalled */ \
    if (!(cond = CreateEvent(NULL, FALSE, FALSE, NULL))) { \
        Py_FatalError("CreateMutex(" #cond ") failed"); };
#define COND_PREPARE(cond) \
    if (!ResetEvent(cond)) { \
        Py_FatalError("ResetEvent(" #cond ") failed"); };
#define COND_SIGNAL(cond) \
    if (!SetEvent(cond)) { \
        Py_FatalError("SetEvent(" #cond ") failed"); };
#define COND_WAIT(cond, mut) \
    { \
        DWORD r; \
        HANDLE objects[2] = { cond, mut }; \
        MUTEX_UNLOCK(mut); \
        r = WaitForMultipleObjects(2, objects, TRUE, INFINITE); \
        if (r != WAIT_OBJECT_0) \
            Py_FatalError("WaitForSingleObject(" #cond ") failed"); \
    }
#define COND_TIMED_WAIT(cond, mut, microseconds, timeout_result) \
    { \
        DWORD r; \
        HANDLE objects[2] = { cond, mut }; \
        MUTEX_UNLOCK(mut); \
        r = WaitForMultipleObjects(2, objects, TRUE, microseconds / 1000); \
        if (r == WAIT_TIMEOUT) { \
            MUTEX_LOCK(mut); \
            timeout_result = 1; \
        } \
        else if (r != WAIT_OBJECT_0) \
            Py_FatalError("WaitForSingleObject(" #cond ") failed"); \
        else \
            timeout_result = 0; \
    }

#else

#error You need either a POSIX-compatible or a Windows system!

#endif /* _POSIX_THREADS, NT_THREADS */


/* Whether the GIL is already taken (-1 if uninitialized). This is volatile
   because it can be read without any lock taken in ceval.c. */
static volatile int gil_locked = -1;
/* Number of GIL switches since the beginning. */
static unsigned long gil_switch_number = 0;
/* Last thread holding / having held the GIL. This helps us know whether
   anyone else was scheduled after we dropped the GIL. */
static PyThreadState *gil_last_holder = NULL;

/* This condition variable allows one or several threads to wait until
   the GIL is released. In addition, the mutex also protects the above
   variables. */
static COND_T gil_cond;
static MUTEX_T gil_mutex;

#ifdef FORCE_SWITCHING
/* This condition variable helps the GIL-releasing thread wait for
   a GIL-awaiting thread to be scheduled and take the GIL. */
static COND_T switch_cond;
static MUTEX_T switch_mutex;
#endif


static int gil_created(void)
{
    return gil_locked >= 0;
}

static void create_gil(void)
{
    MUTEX_INIT(gil_mutex);
#ifdef FORCE_SWITCHING
    MUTEX_INIT(switch_mutex);
#endif
    COND_INIT(gil_cond);
#ifdef FORCE_SWITCHING
    COND_INIT(switch_cond);
#endif
    gil_locked = 0;
    gil_last_holder = NULL;
}

static void recreate_gil(void)
{
    create_gil();
}

static void drop_gil(PyThreadState *tstate)
{
    /* NOTE: tstate is allowed to be NULL. */
    if (!gil_locked)
        Py_FatalError("drop_gil: GIL is not locked");
    if (tstate != NULL && tstate != gil_last_holder)
        Py_FatalError("drop_gil: wrong thread state");

    MUTEX_LOCK(gil_mutex);
    gil_locked = 0;
    COND_SIGNAL(gil_cond);
#ifdef FORCE_SWITCHING
    COND_PREPARE(switch_cond);
#endif
    MUTEX_UNLOCK(gil_mutex);
    
#ifdef FORCE_SWITCHING
    if (gil_drop_request) {
        MUTEX_LOCK(switch_mutex);
        /* Not switched yet => wait */
        if (gil_last_holder == tstate)
            COND_WAIT(switch_cond, switch_mutex);
        MUTEX_UNLOCK(switch_mutex);
    }
#endif
}

static void take_gil(PyThreadState *tstate)
{
    int err;
    if (tstate == NULL)
        Py_FatalError("take_gil: NULL tstate");

    err = errno;
    MUTEX_LOCK(gil_mutex);

    if (!gil_locked)
        goto _ready;
    
    COND_PREPARE(gil_cond);
    while (gil_locked) {
        int timed_out = 0;
        unsigned long saved_switchnum;

        saved_switchnum = gil_switch_number;
        COND_TIMED_WAIT(gil_cond, gil_mutex, INTERVAL, timed_out);
        /* If we timed out and no switch occurred in the meantime, it is time
           to ask the GIL-holding thread to drop it. */
        if (timed_out && gil_locked && gil_switch_number == saved_switchnum) {
            SET_GIL_DROP_REQUEST();
        }
    }
_ready:
#ifdef FORCE_SWITCHING
    /* This mutex must be taken before modifying gil_last_holder (see drop_gil()). */
    MUTEX_LOCK(switch_mutex);
#endif
    /* We now hold the GIL */
    gil_locked = 1;

    if (tstate != gil_last_holder) {
        gil_last_holder = tstate;
        ++gil_switch_number;
    }
#ifdef FORCE_SWITCHING
    COND_SIGNAL(switch_cond);
    MUTEX_UNLOCK(switch_mutex);
#endif
    if (gil_drop_request) {
        RESET_GIL_DROP_REQUEST();
    }
    if (tstate->async_exc != NULL) {
        _PyEval_SignalAsyncExc();
    }
    
    MUTEX_UNLOCK(gil_mutex);
    errno = err;
}

void _PyEval_SetSwitchInterval(unsigned long microseconds)
{
    gil_interval = microseconds;
}

unsigned long _PyEval_GetSwitchInterval()
{
    return gil_interval;
}
