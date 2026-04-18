#ifndef SCHEDULER_H
#define SCHEDULER_H

/*
 * scheduler.h
 * -----------
 * All shared declarations for the Hybrid FaaS Adaptive Scheduler.
 *
 * Compiles on two platforms:
 *   Linux  — compile normally, uses pthreads
 *   xv6    — compile with -DXV6, uses clone()-based threads
 *
 * No floats anywhere. All fractional values are stored as integers
 * multiplied by 10000. Examples:
 *   0.75  stored as  7500
 *   0.005 stored as    50
 *   1.0   stored as 10000
 */

/* ------------------------------------------------------------------ */
/* Platform layer                                                       */
/* ------------------------------------------------------------------ */

#ifdef XV6
    #include "types.h"
    #include "user.h"      /* uptime(), sleep(), thread_create(), thread_join() */
    #include "spinlock.h"  /* struct spinlock, acquire(), release()             */

    typedef struct spinlock  Lock;
    typedef int              ThreadHandle;

    #define LOCK_INIT(lk)                   /* zero-init is enough in xv6 */
    #define LOCK(lk)                        acquire(&(lk))
    #define UNLOCK(lk)                      release(&(lk))
    #define NOW()                           uptime()
    #define SLEEP(t)                        sleep(t)
    #define THREAD_START(fn, arg, stk, sz)  thread_create(fn, arg, stk, sz)
    #define THREAD_WAIT(h)                  thread_join(h)

#else
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>
    #include <unistd.h>
    #include <pthread.h>
    #include <time.h>

    typedef pthread_mutex_t  Lock;
    typedef pthread_t        ThreadHandle;

    #define LOCK_INIT(lk)                   pthread_mutex_init(&(lk), NULL)
    #define LOCK(lk)                        pthread_mutex_lock(&(lk))
    #define UNLOCK(lk)                      pthread_mutex_unlock(&(lk))

    /* Emulate xv6 ticks as 10ms units using CLOCK_MONOTONIC */
    static inline int _now(void) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (int)(ts.tv_sec * 100 + ts.tv_nsec / 10000000);
    }
    #define NOW()                           _now()
    #define SLEEP(t)                        usleep((unsigned int)((t) * 10000))
    #define THREAD_START(fn, arg, stk, sz)  \
        ({ ThreadHandle _h; pthread_create(&_h, NULL, fn, arg); _h; })
    #define THREAD_WAIT(h)                  pthread_join((h), NULL)

#endif /* XV6 */

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

#define MAX_ITER        10000000  /* normalization ceiling                      */
#define ANT_THRESHOLD   50        /* weight <= 50  =>  ANT task  (equiv 0.005) */

#define NUM_WORKERS     8         /* persistent threads for MONSTER tasks       */
#define FAAS_POOL_SIZE  16        /* pre-spawned threads for ANT tasks          */

/* Timing intervals — in ticks, where 1 tick ≈ 10ms */
#define POLL_TICKS      1
#define AGING_TICKS     50        /* aging pass every ~500ms  */
#define MONITOR_TICKS   100       /* adaptive check every ~1s */
#define DLB_TICKS       20        /* load balance every ~200ms */

/* Aging factor — mutated at runtime by the adaptive monitor */
#define AGING_INIT      10        /* starting value (0.001 scaled) */
#define AGING_MIN       5
#define AGING_MAX       50
#define AGING_STEP      1

/* Adaptive monitor targets */
#define TARGET_WAIT     200       /* max acceptable Monster wait (ticks)       */
#define TARGET_UTIL     7000      /* desired worker utilisation (0.70 scaled)  */

/* Cold-start range in ticks for FaaS threads */
#define COLD_MIN        10        /* ~100ms */
#define COLD_MAX        300       /* ~3s    */
#define COLD_IDLE       30        /* idle longer than this => thread is cold   */

/* Fixed array sizes — no dynamic allocation */
#define HEAP_CAP        12000
#define METRICS_CAP     12000
#define LOG_CAP         1000
#define STACK_SZ        4096      /* per-thread stack for xv6 */

/* Task type */
#define TYPE_ANT        0
#define TYPE_MONSTER    1

/* FaaS thread state */
#define STATE_WARM      0
#define STATE_BUSY      1

/* ------------------------------------------------------------------ */
/* Data structures                                                      */
/* ------------------------------------------------------------------ */

/*
 * Task — one unit of work flowing through the scheduler.
 * weight and effective_priority are scaled x10000.
 */
typedef struct {
    int id;
    int iterations;
    int weight;              /* (iterations * 10000) / MAX_ITER */
    int effective_priority;  /* starts = weight; aging reduces it */
    int type;                /* TYPE_ANT or TYPE_MONSTER          */
    int arrival_tick;
    int start_tick;
    int end_tick;
} Task;

/*
 * Metric — one record per completed task, written to metrics.csv.
 */
typedef struct {
    int task_id;
    int type;
    int weight;
    int wait_ticks;          /* scheduling latency (excludes cold-start) */
    int exec_ticks;          /* pure execution time                      */
    int turnaround_ticks;    /* total time from arrival to completion     */
    int cold_ticks;          /* cold-start penalty; 0 for workers        */
} Metric;

/*
 * Snapshot — one observation by the adaptive monitor, written to state_log.csv.
 * Plotting aging_factor over timestamp shows self-tuning behaviour.
 */
typedef struct {
    int tick;
    int avg_monster_wait;
    int worker_util;         /* (busy * 10000) / NUM_WORKERS */
    int ant_qlen;
    int monster_qlen;
    int aging_factor;
} Snapshot;

/*
 * Heap — binary min-heap ordered by effective_priority ascending.
 * The task with the lowest effective_priority sits at index 0.
 * Caller must hold the matching lock before any heap operation.
 */
typedef struct {
    Task data[HEAP_CAP];
    int  size;
} Heap;

/*
 * FaaSThread — one slot in the ANT execution pool.
 * Dispatcher sets current_task then has_task=1 to wake the thread.
 */
typedef struct {
    ThreadHandle handle;
    int          state;       /* STATE_WARM or STATE_BUSY */
    int          last_tick;   /* tick when last task finished */
    Task         current_task;
    int          has_task;    /* 1 = task waiting to run, 0 = idle */
    int          index;
    char         stack[STACK_SZ];
} FaaSThread;

/*
 * Worker — one slot in the MONSTER execution pool.
 * Same has_task pattern as FaaSThread. Never cold-starts.
 */
typedef struct {
    ThreadHandle handle;
    Task         current_task;
    int          has_task;
    int          capacity;    /* always 10000 */
    int          index;
    char         stack[STACK_SZ];
} Worker;

/*
 * Scheduler — the single shared struct all threads point to.
 *
 * Lock acquisition order when two locks are needed simultaneously:
 *   ant_lock  THEN  monster_lock   (consistent order prevents deadlock)
 */
typedef struct {
    Heap       ant_q;
    Heap       monster_q;

    FaaSThread faas[FAAS_POOL_SIZE];
    Worker     workers[NUM_WORKERS];

    int        running;       /* 1 = alive, 0 = shutdown */
    int        aging_factor;  /* tuned at runtime by adaptive monitor */

    Metric     metrics[METRICS_CAP];
    int        metric_count;

    Snapshot   log[LOG_CAP];
    int        log_count;

    /* One lock per shared resource */
    Lock       ant_lock;
    Lock       monster_lock;
    Lock       faas_lock;
    Lock       metric_lock;
    Lock       log_lock;
    Lock       aging_lock;
    Lock       run_lock;

    /* Driver thread stacks (xv6 requires explicit stack allocation) */
    char       stk_arrival[STACK_SZ];
    char       stk_aging[STACK_SZ];
    char       stk_monitor[STACK_SZ];
    char       stk_dlb[STACK_SZ];
    char       stk_router[STACK_SZ];
    char       stk_shutdown[STACK_SZ];
} Scheduler;

/*
 * ThreadArg — passed to worker and FaaS threads.
 * Bundles scheduler pointer + pool index into one void* arg.
 */
typedef struct {
    Scheduler *s;
    int        index;
} ThreadArg;

/* Forward declaration — TaskSource is defined in main.c */
struct TaskSource;

/*
 * ArrivalArg — passed to the arrival_driver thread.
 */
typedef struct {
    Scheduler        *s;
    struct TaskSource *src;
} ArrivalArg;

/* ------------------------------------------------------------------ */
/* Function declarations                                               */
/* ------------------------------------------------------------------ */

/* heap.c */
void  heap_push(Heap *h, Task t);
Task  heap_pop(Heap *h);
Task *heap_top(Heap *h);
void  heap_rebuild(Heap *h);
int   heap_empty(Heap *h);

/* scheduler.c */
void      sched_init(Scheduler *s);
Task      classify(Task t);
void      submit(Scheduler *s, Task *tasks, int n);
int       faas_is_cold(FaaSThread *f);
int       cold_start(void);
int       faas_dispatch(Scheduler *s, Task t);
int       worker_assign(Scheduler *s, Task t);
void      do_aging(Scheduler *s);
Snapshot  do_observe(Scheduler *s);
void      do_adjust(Scheduler *s, Snapshot snap);
void      do_rebalance(Scheduler *s);
int       sched_alive(Scheduler *s);

/* Thread entry points */
void *faas_thread_fn(void *arg);
void *worker_thread_fn(void *arg);
void *arrival_driver(void *arg);
void *aging_driver(void *arg);
void *monitor_driver(void *arg);
void *dlb_driver(void *arg);
void *router_driver(void *arg);
void *shutdown_driver(void *arg);

/* main.c */
int  lcg_rand(void);
int  rand_range(int lo, int hi);
void report(Scheduler *s);

int  src_has_more(struct TaskSource *src);
int  src_fetch(struct TaskSource *src, Task *out, int max);
int  src_streaming(struct TaskSource *src);

#endif /* SCHEDULER_H */
