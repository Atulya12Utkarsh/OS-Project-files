/*
 * main.c
 * ------
 * Task generation, program entry point, and CSV report.
 *
 * Workload:
 *   9000 ANT tasks:     10,000 – 50,000 iterations  (~1ms each)
 *   1000 MONSTER tasks: 5,000,000 – 10,000,000 iterations  (~100ms+)
 *
 * Three-phase arrival:
 *   Phase 1 — Flood:    5000 tasks submitted at once
 *   Phase 2 — Trickle:  100 tasks every 50ms for 40 rounds (4000 tasks)
 *   Phase 3 — Burst:    remaining 1000 tasks at once
 *
 * Output files (Linux):
 *   metrics.csv    one row per completed task
 *   state_log.csv  one row per adaptive monitor snapshot
 *
 * Output on xv6:
 *   Both CSVs printed to stdout — redirect with > filename.
 *
 * Build (Linux):
 *   gcc -Wall -g -o scheduler main.c scheduler.c heap.c -lpthread
 *
 * Build (xv6):
 *   Copy all four files into the xv6 source tree.
 *   Add _scheduler to UPROGS in xv6 Makefile.
 *   Rebuild with: make CFLAGS+='-DXV6' qemu-nox
 */

#include "scheduler.h"

/* ================================================================== */
/* TASK SOURCE                                                         */
/* Generates and delivers tasks in three phases.                      */
/* Fixed-size arrays — no malloc.                                     */
/* ================================================================== */

#define N_ANT       9000
#define N_MONSTER   1000
#define N_TOTAL     (N_ANT + N_MONSTER)

#define PH1_SIZE    5000     /* flood  */
#define PH2_ROUNDS  40       /* trickle: 40 * 100 = 4000 tasks */
#define PH2_BATCH   100
/* Phase 3 sends whatever remains */

#define ANT_LO      10000
#define ANT_HI      50000
#define MON_LO      5000000
#define MON_HI      10000000

typedef struct TaskSource {
    Task all[N_TOTAL];  /* all tasks pre-generated at startup */
    int  total;
    int  phase;         /* 1, 2, or 3 */
    int  ph2_round;
    int  next;          /* next index into all[] */
    int  streaming;     /* 1 during trickle phase */
} TaskSource;

/*
 * src_init
 * Pre-generates all tasks with random iteration counts.
 * Shuffles the array so each phase gets a realistic mix of ANTs and Monsters.
 */
static void src_init(TaskSource *src) {
    int i, j;
    Task tmp;

    memset(src, 0, sizeof(TaskSource));

    /* Generate ANT tasks */
    for (i = 0; i < N_ANT; i++) {
        src->all[i].id         = i + 1;
        src->all[i].iterations = rand_range(ANT_LO, ANT_HI);
    }

    /* Generate MONSTER tasks */
    for (i = 0; i < N_MONSTER; i++) {
        int idx = N_ANT + i;
        src->all[idx].id         = idx + 1;
        src->all[idx].iterations = rand_range(MON_LO, MON_HI);
    }

    src->total     = N_TOTAL;
    src->phase     = 1;
    src->next      = 0;
    src->streaming = 0;

    /* Fisher-Yates shuffle for realistic mixed arrival */
    for (i = N_TOTAL - 1; i > 0; i--) {
        j          = rand_range(0, i);
        tmp        = src->all[i];
        src->all[i] = src->all[j];
        src->all[j] = tmp;
        src->all[i].id = i + 1;   /* re-assign IDs after shuffle */
    }
    src->all[0].id = 1;
}

/* Returns 1 while there are undelivered tasks */
int src_has_more(TaskSource *src) {
    return src->next < src->total;
}

/* Returns 1 during Phase 2 (trickle) */
int src_streaming(TaskSource *src) {
    return src->streaming;
}

/*
 * src_fetch
 * Copies the next batch into out[] and returns the count.
 * Manages phase transitions internally.
 */
int src_fetch(TaskSource *src, Task *out, int max) {
    int n = 0, i;

    if (src->phase == 1) {
        /* Phase 1 — Flood: deliver PH1_SIZE tasks at once */
        n = PH1_SIZE;
        if (n > max) n = max;
        for (i = 0; i < n && src->next < src->total; i++)
            out[i] = src->all[src->next++];
        src->phase      = 2;
        src->streaming  = 1;
        src->ph2_round  = 0;

    } else if (src->phase == 2) {
        /* Phase 2 — Trickle: PH2_BATCH tasks per call */
        if (src->ph2_round >= PH2_ROUNDS) {
            src->phase     = 3;
            src->streaming = 0;
            return 0;   /* tell arrival_driver to call again for Phase 3 */
        }
        n = PH2_BATCH;
        if (n > max) n = max;
        for (i = 0; i < n && src->next < src->total; i++)
            out[i] = src->all[src->next++];
        src->ph2_round++;

    } else if (src->phase == 3) {
        /* Phase 3 — Burst: everything remaining */
        n = src->total - src->next;
        if (n > max) n = max;
        for (i = 0; i < n; i++)
            out[i] = src->all[src->next++];
    }

    return n;
}

/* ================================================================== */
/* REPORT                                                              */
/* Prints a console summary and writes two CSV files.                 */
/* ================================================================== */

void report(Scheduler *s) {
    int   i;
    long  aw = 0, ae = 0, at = 0, ac = 0;   /* ANT totals     */
    long  mw = 0, me = 0, mt = 0;            /* MONSTER totals */
    int   an = 0, mn = 0;

    for (i = 0; i < s->metric_count; i++) {
        Metric *m = &s->metrics[i];
        if (m->type == TYPE_ANT) {
            aw += m->wait_ticks;
            ae += m->exec_ticks;
            at += m->turnaround_ticks;
            ac += m->cold_ticks;
            an++;
        } else {
            mw += m->wait_ticks;
            me += m->exec_ticks;
            mt += m->turnaround_ticks;
            mn++;
        }
    }

#ifndef XV6
    printf("\n=== Hybrid FaaS Adaptive Scheduler — Results ===\n");
    printf("ANT tasks (via FaaS threads):\n");
    printf("  count          : %d\n",   an);
    printf("  avg wait       : %ld ticks\n", an ? aw/an : 0L);
    printf("  avg exec       : %ld ticks\n", an ? ae/an : 0L);
    printf("  avg cold-start : %ld ticks\n", an ? ac/an : 0L);
    printf("  avg turnaround : %ld ticks\n", an ? at/an : 0L);
    printf("MONSTER tasks (persistent workers):\n");
    printf("  count          : %d\n",   mn);
    printf("  avg wait       : %ld ticks\n", mn ? mw/mn : 0L);
    printf("  avg exec       : %ld ticks\n", mn ? me/mn : 0L);
    printf("  avg turnaround : %ld ticks\n", mn ? mt/mn : 0L);
    printf("Overall: %d tasks, %d monitor snapshots\n",
           s->metric_count, s->log_count);
    printf("=================================================\n\n");
#endif

#ifndef XV6
    /* Write metrics.csv */
    {
        FILE *f = fopen("metrics.csv", "w");
        if (f) {
            fprintf(f, "task_id,type,weight,wait_ticks,"
                       "exec_ticks,turnaround_ticks,cold_ticks\n");
            for (i = 0; i < s->metric_count; i++) {
                Metric *m = &s->metrics[i];
                fprintf(f, "%d,%s,%d,%d,%d,%d,%d\n",
                    m->task_id,
                    m->type == TYPE_ANT ? "ANT" : "MONSTER",
                    m->weight,
                    m->wait_ticks, m->exec_ticks,
                    m->turnaround_ticks, m->cold_ticks);
            }
            fclose(f);
            printf("Written: metrics.csv  (%d rows)\n", s->metric_count);
        }
    }

    /* Write state_log.csv */
    {
        FILE *f = fopen("state_log.csv", "w");
        if (f) {
            fprintf(f, "tick,aging_factor,avg_monster_wait,"
                       "worker_util,ant_qlen,monster_qlen\n");
            for (i = 0; i < s->log_count; i++) {
                Snapshot *snap = &s->log[i];
                fprintf(f, "%d,%d,%d,%d,%d,%d\n",
                    snap->tick, snap->aging_factor,
                    snap->avg_monster_wait, snap->worker_util,
                    snap->ant_qlen, snap->monster_qlen);
            }
            fclose(f);
            printf("Written: state_log.csv  (%d rows)\n", s->log_count);
        }
    }
#else
    /* xv6: dump metrics CSV to stdout */
    printf("task_id,type,weight,wait_ticks,exec_ticks,turnaround_ticks,cold_ticks\n");
    for (i = 0; i < s->metric_count; i++) {
        Metric *m = &s->metrics[i];
        printf("%d,%d,%d,%d,%d,%d,%d\n",
               m->task_id, m->type, m->weight,
               m->wait_ticks, m->exec_ticks,
               m->turnaround_ticks, m->cold_ticks);
    }
#endif
}

/* ================================================================== */
/* MAIN                                                                */
/*                                                                     */
/* Steps:                                                              */
/*   1. Initialise Scheduler                                           */
/*   2. Spawn persistent worker threads (MONSTER pool)                */
/*   3. Spawn FaaS threads (ANT pool)                                 */
/*   4. Spawn all six driver threads                                   */
/*   5. Busy-wait until shutdown_driver flips running = 0             */
/*   6. Join worker and FaaS threads                                   */
/*   7. Write CSV output                                               */
/*                                                                     */
/* Scheduler and TaskSource are static globals — avoids large stack   */
/* frames which cause stack overflow on xv6.                          */
/* ================================================================== */

static Scheduler  S;
static TaskSource SRC;
static ThreadArg  faas_args[FAAS_POOL_SIZE];
static ThreadArg  wkr_args[NUM_WORKERS];
static ArrivalArg arr_arg;

int main(void) {
    int i;

    /* --- 1. Initialise --- */
    sched_init(&S);
    src_init(&SRC);

#ifndef XV6
    printf("[MAIN] scheduler ready — %d ANT + %d MONSTER = %d tasks\n",
           N_ANT, N_MONSTER, N_TOTAL);
    printf("[MAIN] pools: %d workers, %d FaaS threads\n",
           NUM_WORKERS, FAAS_POOL_SIZE);
#endif

    /* --- 2. Persistent worker threads (MONSTER pool) --- */
    for (i = 0; i < NUM_WORKERS; i++) {
        wkr_args[i].s     = &S;
        wkr_args[i].index = i;
        S.workers[i].handle = THREAD_START(worker_thread_fn,
                                            &wkr_args[i],
                                            S.workers[i].stack,
                                            STACK_SZ);
    }

    /* --- 3. FaaS threads (ANT pool) --- */
    for (i = 0; i < FAAS_POOL_SIZE; i++) {
        faas_args[i].s     = &S;
        faas_args[i].index = i;
        S.faas[i].handle = THREAD_START(faas_thread_fn,
                                         &faas_args[i],
                                         S.faas[i].stack,
                                         STACK_SZ);
    }

    /* --- 4. Driver threads --- */
    arr_arg.s   = &S;
    arr_arg.src = &SRC;
    THREAD_START(arrival_driver,  &arr_arg,  S.stk_arrival,  STACK_SZ);
    THREAD_START(aging_driver,    &S,         S.stk_aging,    STACK_SZ);
    THREAD_START(monitor_driver,  &S,         S.stk_monitor,  STACK_SZ);
    THREAD_START(dlb_driver,      &S,         S.stk_dlb,      STACK_SZ);
    THREAD_START(router_driver,   &S,         S.stk_router,   STACK_SZ);
    THREAD_START(shutdown_driver, &S,         S.stk_shutdown, STACK_SZ);

#ifndef XV6
    printf("[MAIN] all threads running\n");
#endif

    /* --- 5. Wait for shutdown --- */
    while (sched_alive(&S))
        SLEEP(POLL_TICKS);

#ifndef XV6
    printf("[MAIN] shutdown — joining threads\n");
#endif

    /* --- 6. Join worker and FaaS threads --- */
    for (i = 0; i < NUM_WORKERS; i++)
        THREAD_WAIT(S.workers[i].handle);
    for (i = 0; i < FAAS_POOL_SIZE; i++)
        THREAD_WAIT(S.faas[i].handle);

    /* --- 7. Output --- */
    report(&S);

    return 0;
}
