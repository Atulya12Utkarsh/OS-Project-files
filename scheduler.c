/*
 * scheduler.c
 * -----------
 * Core scheduling logic for the Hybrid FaaS Adaptive Scheduler.
 *
 * Sections:
 *   1.  Random number generator  (LCG, no rand() on xv6)
 *   2.  Scheduler initialisation
 *   3.  Task classification and submission
 *   4.  Cold-start model
 *   5.  FaaS dispatcher  +  FaaS worker thread
 *   6.  Aging driver  (effective_priority decay)
 *   7.  Adaptive monitor  (observe -> evaluate -> adjust loop)
 *   8.  Hybrid router  (routes tasks to FaaS or worker pool)
 *   9.  Persistent worker thread
 *   10. Dynamic load balancer driver
 *   11. Arrival driver
 *   12. Shutdown controller
 *   13. sched_alive helper
 */

#include "scheduler.h"

/* ================================================================== */
/* 1. RANDOM NUMBER GENERATOR                                          */
/* xv6 has no rand(). A Linear Congruential Generator is enough       */
/* for simulating cold-start delays.                                   */
/* ================================================================== */

static unsigned int _seed = 12345;

int lcg_rand(void) {
    _seed = (unsigned int)(1664525UL * _seed + 1013904223UL) & 0x7fffffff;
    return (int)_seed;
}

/* Return a random integer in [lo, hi] inclusive */
int rand_range(int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + (lcg_rand() % (hi - lo + 1));
}

/* ================================================================== */
/* 2. SCHEDULER INITIALISATION                                         */
/* Zero out everything, init all locks, set initial values.            */
/* Call this once before spawning any thread.                          */
/* ================================================================== */

void sched_init(Scheduler *s) {
    int i;

    /* Zero the entire struct — sets all counts, ticks, flags to 0 */
    memset(s, 0, sizeof(Scheduler));

    s->running      = 1;
    s->aging_factor = AGING_INIT;

    /*
     * Initialise all locks.
     * On xv6: spinlocks are zero-initialised by memset, LOCK_INIT is a no-op.
     * On Linux: pthread_mutex_init must be called explicitly.
     */
    LOCK_INIT(s->ant_lock);
    LOCK_INIT(s->monster_lock);
    LOCK_INIT(s->faas_lock);
    LOCK_INIT(s->metric_lock);
    LOCK_INIT(s->log_lock);
    LOCK_INIT(s->aging_lock);
    LOCK_INIT(s->run_lock);

    /* Set up FaaS thread slots */
    for (i = 0; i < FAAS_POOL_SIZE; i++) {
        s->faas[i].index     = i;
        s->faas[i].state     = STATE_WARM;
        s->faas[i].last_tick = NOW();
        s->faas[i].has_task  = 0;
    }

    /* Set up persistent worker slots */
    for (i = 0; i < NUM_WORKERS; i++) {
        s->workers[i].index    = i;
        s->workers[i].capacity = 10000;   /* scaled 1.0 */
        s->workers[i].has_task = 0;
    }
}

/* ================================================================== */
/* 3. TASK CLASSIFICATION AND SUBMISSION                               */
/* ================================================================== */

/*
 * classify
 * Computes weight = (iterations * 10000) / MAX_ITER.
 * Assigns TYPE_ANT if weight <= ANT_THRESHOLD, otherwise TYPE_MONSTER.
 * Sets effective_priority = weight as the starting value.
 * Pure computation — no shared state, no lock needed.
 */
Task classify(Task t) {
    t.weight             = (int)(((long)t.iterations * 10000) / MAX_ITER);
    t.effective_priority = t.weight;
    t.type               = (t.weight <= ANT_THRESHOLD) ? TYPE_ANT : TYPE_MONSTER;
    return t;
}

/*
 * submit
 * Single entry point for all incoming tasks.
 * Classifies each task, stamps arrival_tick, inserts into the right heap.
 * Works for any batch size — 1 task or 10000.
 */
void submit(Scheduler *s, Task *tasks, int n) {
    int i;
    for (i = 0; i < n; i++) {
        Task t         = classify(tasks[i]);
        t.arrival_tick = NOW();

        if (t.type == TYPE_ANT) {
            LOCK(s->ant_lock);
                heap_push(&s->ant_q, t);
            UNLOCK(s->ant_lock);
        } else {
            LOCK(s->monster_lock);
                heap_push(&s->monster_q, t);
            UNLOCK(s->monster_lock);
        }
    }
}

/* ================================================================== */
/* 4. COLD-START MODEL                                                 */
/*                                                                     */
/* A FaaS thread is "cold" when it has been idle longer than          */
/* COLD_IDLE ticks. On wakeup it sleeps a random number of ticks      */
/* before executing — this models a serverless container that lost    */
/* its warm state after idling too long.                               */
/* ================================================================== */

/* Returns 1 if the FaaS thread has been idle long enough to be cold */
int faas_is_cold(FaaSThread *f) {
    return (NOW() - f->last_tick) > COLD_IDLE;
}

/* Sleep a random number of ticks and return the actual delay.
 * The delay is stored separately in the metric so cold-start cost
 * is visible independently from scheduling wait time. */
int cold_start(void) {
    int delay = rand_range(COLD_MIN, COLD_MAX);
    SLEEP(delay);
    return delay;
}

/* ================================================================== */
/* 5. FAAS DISPATCHER + FAAS WORKER THREAD                            */
/* ================================================================== */

/*
 * faas_dispatch
 * Finds a free FaaS thread — prefers WARM ones first.
 * Assigns the task via direct struct write (shared memory — no IPC).
 * Sets has_task=1 to wake the thread's busy-wait loop.
 * Returns 1 on success, 0 if all threads are busy (caller re-queues).
 */
int faas_dispatch(Scheduler *s, Task t) {
    int i;
    FaaSThread *best = (FaaSThread *)0;

    LOCK(s->faas_lock);

        /* First pass — prefer a warm idle thread */
        for (i = 0; i < FAAS_POOL_SIZE; i++) {
            if (s->faas[i].state == STATE_WARM &&
                !s->faas[i].has_task &&
                !faas_is_cold(&s->faas[i])) {
                best = &s->faas[i];
                break;
            }
        }

        /* Second pass — accept any idle thread (may be cold) */
        if (!best) {
            for (i = 0; i < FAAS_POOL_SIZE; i++) {
                if (s->faas[i].state == STATE_WARM && !s->faas[i].has_task) {
                    best = &s->faas[i];
                    break;
                }
            }
        }

        if (!best) {
            UNLOCK(s->faas_lock);
            return 0;   /* pool is fully busy */
        }

        best->current_task = t;     /* direct write — shared memory */
        best->state        = STATE_BUSY;
        best->has_task     = 1;     /* wakes the thread's busy-wait */

    UNLOCK(s->faas_lock);
    return 1;
}

/*
 * faas_thread_fn
 * Entry point for each pre-spawned FaaS thread.
 *
 * Loop:
 *   1. Check shutdown
 *   2. Busy-wait on has_task flag
 *   3. Apply cold-start penalty if thread was idle too long
 *   4. Execute the iteration loop (simulated work)
 *   5. Record metric
 *   6. Reset state, go back to step 1
 */
void *faas_thread_fn(void *arg) {
    ThreadArg  *ta   = (ThreadArg *)arg;
    Scheduler  *s    = ta->s;
    FaaSThread *self = &s->faas[ta->index];
    int   ct;
    long  i;
    Task  t;

    while (1) {
        /* Check shutdown — exit cleanly if no pending task */
        LOCK(s->run_lock);
        if (!s->running && !self->has_task) {
            UNLOCK(s->run_lock);
            return (void *)0;
        }
        UNLOCK(s->run_lock);

        /* Busy-wait: poll has_task until dispatcher sets it */
        if (!self->has_task) {
            SLEEP(POLL_TICKS);
            continue;
        }

        t  = self->current_task;   /* copy task from shared struct */
        ct = 0;

        /* Apply cold-start penalty if this thread has been idle too long */
        if (faas_is_cold(self)) {
            ct = cold_start();
            /* thread is now warm again after simulated container init */
        }

        /* Record when actual execution begins (after cold-start) */
        t.start_tick = NOW();

        /* Simulated workload: iterate t.iterations times */
        for (i = 0; i < (long)t.iterations; i++) { /* tight loop */ }

        t.end_tick = NOW();

        /* Mark thread as free and warm */
        LOCK(s->faas_lock);
            self->last_tick = NOW();
            self->state     = STATE_WARM;
            self->has_task  = 0;
        UNLOCK(s->faas_lock);

        /*
         * Record metric.
         * wait_ticks excludes cold_ticks so scheduling latency and
         * cold-start cost appear as separate columns in the CSV.
         */
        LOCK(s->metric_lock);
            if (s->metric_count < METRICS_CAP) {
                Metric *m        = &s->metrics[s->metric_count++];
                m->task_id       = t.id;
                m->type          = TYPE_ANT;
                m->weight        = t.weight;
                m->wait_ticks    = (t.start_tick - t.arrival_tick) - ct;
                m->exec_ticks    = t.end_tick - t.start_tick;
                m->turnaround_ticks = t.end_tick - t.arrival_tick;
                m->cold_ticks    = ct;
            }
        UNLOCK(s->metric_lock);
    }
    return (void *)0;
}

/* ================================================================== */
/* 6. AGING DRIVER                                                     */
/*                                                                     */
/* Every AGING_TICKS ticks, decays effective_priority for every       */
/* task currently waiting in either queue:                             */
/*                                                                     */
/*   reduction = (aging_factor * wait_ticks) / 10000                  */
/*   new_priority = MAX(0, weight - reduction)                         */
/*                                                                     */
/* Lower effective_priority = higher urgency in the min-heap.         */
/* A Monster waiting long enough will eventually surface above newly   */
/* arrived ANT tasks — this is the starvation prevention mechanism.   */
/*                                                                     */
/* The aging_factor is read once per pass under its lock so the        */
/* adaptive monitor's latest value is always picked up.               */
/* ================================================================== */

void do_aging(Scheduler *s) {
    int i, wait, reduction, np, factor;
    int now = NOW();

    /* Read aging_factor under lock once — avoids partial update mid-loop */
    LOCK(s->aging_lock);
        factor = s->aging_factor;
    UNLOCK(s->aging_lock);

    /* Age all tasks in the ANT queue */
    LOCK(s->ant_lock);
        for (i = 0; i < s->ant_q.size; i++) {
            wait      = now - s->ant_q.data[i].arrival_tick;
            reduction = (factor * wait) / 10000;
            np        = s->ant_q.data[i].weight - reduction;
            s->ant_q.data[i].effective_priority = (np > 0) ? np : 0;
        }
        heap_rebuild(&s->ant_q);
    UNLOCK(s->ant_lock);

    /* Age all tasks in the MONSTER queue */
    LOCK(s->monster_lock);
        for (i = 0; i < s->monster_q.size; i++) {
            wait      = now - s->monster_q.data[i].arrival_tick;
            reduction = (factor * wait) / 10000;
            np        = s->monster_q.data[i].weight - reduction;
            s->monster_q.data[i].effective_priority = (np > 0) ? np : 0;
        }
        heap_rebuild(&s->monster_q);
    UNLOCK(s->monster_lock);
}

/* Aging driver thread — fires every AGING_TICKS */
void *aging_driver(void *arg) {
    Scheduler *s = (Scheduler *)arg;
    while (1) {
        SLEEP(AGING_TICKS);
        if (!sched_alive(s)) return (void *)0;
        do_aging(s);
    }
    return (void *)0;
}

/* ================================================================== */
/* 7. ADAPTIVE MONITOR                                                 */
/*                                                                     */
/* The feedback loop that makes the scheduler genuinely adaptive.     */
/* Every MONITOR_TICKS ticks it:                                       */
/*   1. Observes  — snapshots Monster wait time + worker utilisation  */
/*   2. Evaluates — checks against TARGET_WAIT and TARGET_UTIL        */
/*   3. Adjusts   — nudges aging_factor up or down                    */
/*   4. Logs      — appends Snapshot to log[] for CSV export          */
/*                                                                     */
/* The aging_driver picks up the new aging_factor automatically on     */
/* its very next pass — no extra coordination needed.                 */
/* ================================================================== */

/*
 * do_observe
 * Non-destructive snapshot of the current system state.
 */
Snapshot do_observe(Scheduler *s) {
    Snapshot snap;
    int i, total, mc, busy, now;

    memset(&snap, 0, sizeof(Snapshot));
    now = NOW();
    snap.tick = now;

    /* Average Monster wait time */
    LOCK(s->monster_lock);
        mc    = s->monster_q.size;
        total = 0;
        for (i = 0; i < mc; i++)
            total += now - s->monster_q.data[i].arrival_tick;
        snap.monster_qlen = mc;
    UNLOCK(s->monster_lock);

    snap.avg_monster_wait = (mc > 0) ? (total / mc) : 0;

    /* Worker utilisation: fraction of persistent workers executing */
    busy = 0;
    for (i = 0; i < NUM_WORKERS; i++)
        if (s->workers[i].has_task) busy++;
    snap.worker_util = (busy * 10000) / NUM_WORKERS;

    LOCK(s->ant_lock);
        snap.ant_qlen = s->ant_q.size;
    UNLOCK(s->ant_lock);

    LOCK(s->aging_lock);
        snap.aging_factor = s->aging_factor;
    UNLOCK(s->aging_lock);

    return snap;
}

/*
 * do_adjust
 * Three conditions:
 *
 * 1) Monsters waiting too long (starvation risk):
 *    Increase aging_factor — effective_priority decays faster, Monsters
 *    surface sooner in the heap.
 *
 * 2) Workers underutilised AND Monsters not starving:
 *    Decrease aging_factor — favour ANTs more, keep throughput high.
 *
 * 3) Balanced — no change.
 */
void do_adjust(Scheduler *s, Snapshot snap) {
    LOCK(s->aging_lock);

        if (snap.avg_monster_wait > TARGET_WAIT) {
            s->aging_factor += AGING_STEP;
            if (s->aging_factor > AGING_MAX)
                s->aging_factor = AGING_MAX;

        } else if (snap.worker_util < TARGET_UTIL &&
                   snap.avg_monster_wait < TARGET_WAIT / 2) {
            s->aging_factor -= AGING_STEP;
            if (s->aging_factor < AGING_MIN)
                s->aging_factor = AGING_MIN;
        }

    UNLOCK(s->aging_lock);
}

/* Adaptive monitor driver thread */
void *monitor_driver(void *arg) {
    Scheduler *s = (Scheduler *)arg;
    Snapshot snap;

    while (1) {
        SLEEP(MONITOR_TICKS);
        if (!sched_alive(s)) return (void *)0;

        snap = do_observe(s);      /* step 1: observe  */
        do_adjust(s, snap);        /* steps 2+3: evaluate and adjust */

        /* step 4: log snapshot for CSV */
        LOCK(s->log_lock);
            if (s->log_count < LOG_CAP)
                s->log[s->log_count++] = snap;
        UNLOCK(s->log_lock);
    }
    return (void *)0;
}

/* ================================================================== */
/* 8. HYBRID ROUTER                                                    */
/*                                                                     */
/* Dequeues the next task and routes it:                               */
/*   TYPE_ANT     -> FaaS thread pool   (faas_dispatch)               */
/*   TYPE_MONSTER -> persistent workers (worker_assign)               */
/*                                                                     */
/* pick_next compares effective_priority across both queues. Aging    */
/* ensures Monsters eventually win the comparison — no starvation.    */
/* If the target pool is full the task goes back into its queue.      */
/* ================================================================== */

/*
 * pick_next
 * Peek at both queue heads, dequeue the one with lower effective_priority.
 * Lock order: ALWAYS ant_lock first, THEN monster_lock.
 */
static Task pick_next(Scheduler *s) {
    Task empty, result;
    Task *at, *mt;

    memset(&empty, 0, sizeof(Task));
    empty.id = -1;   /* sentinel: "nothing to serve" */

    LOCK(s->ant_lock);
    LOCK(s->monster_lock);

        at = heap_top(&s->ant_q);
        mt = heap_top(&s->monster_q);

        if      (!at && !mt)                                    result = empty;
        else if (!at)                                           result = heap_pop(&s->monster_q);
        else if (!mt)                                           result = heap_pop(&s->ant_q);
        else if (at->effective_priority <= mt->effective_priority) result = heap_pop(&s->ant_q);
        else                                                    result = heap_pop(&s->monster_q);

    UNLOCK(s->monster_lock);
    UNLOCK(s->ant_lock);

    return result;
}

/*
 * worker_assign
 * Find the first idle worker and assign the task via shared memory.
 * Sets has_task=1 to wake its busy-wait loop.
 * Returns 1 on success, 0 if all workers are busy.
 */
int worker_assign(Scheduler *s, Task t) {
    int i;
    for (i = 0; i < NUM_WORKERS; i++) {
        if (!s->workers[i].has_task) {
            s->workers[i].current_task = t;
            s->workers[i].has_task     = 1;
            return 1;
        }
    }
    return 0;
}

/* Hybrid router driver thread */
void *router_driver(void *arg) {
    Scheduler *s = (Scheduler *)arg;
    Task t;
    int  ok;

    while (1) {
        if (!sched_alive(s)) return (void *)0;

        t = pick_next(s);

        if (t.id == -1) {
            SLEEP(POLL_TICKS);
            continue;
        }

        if (t.type == TYPE_ANT) {
            ok = faas_dispatch(s, t);
            if (!ok) {
                /* FaaS pool full — put task back */
                LOCK(s->ant_lock);
                    heap_push(&s->ant_q, t);
                UNLOCK(s->ant_lock);
            }
        } else {
            ok = worker_assign(s, t);
            if (!ok) {
                /* Worker pool full — put task back */
                LOCK(s->monster_lock);
                    heap_push(&s->monster_q, t);
                UNLOCK(s->monster_lock);
            }
        }
    }
    return (void *)0;
}

/* ================================================================== */
/* 9. PERSISTENT WORKER THREAD                                         */
/*                                                                     */
/* Handles MONSTER tasks. Never cold-starts. Same busy-wait pattern   */
/* as FaaS thread: poll has_task, execute, record metric, repeat.     */
/* ================================================================== */

void *worker_thread_fn(void *arg) {
    ThreadArg *ta   = (ThreadArg *)arg;
    Scheduler *s    = ta->s;
    Worker    *self = &s->workers[ta->index];
    long       i;
    Task       t;

    while (1) {
        LOCK(s->run_lock);
        if (!s->running && !self->has_task) {
            UNLOCK(s->run_lock);
            return (void *)0;
        }
        UNLOCK(s->run_lock);

        if (!self->has_task) {
            SLEEP(POLL_TICKS);
            continue;
        }

        t            = self->current_task;
        t.start_tick = NOW();

        /* Simulated work */
        for (i = 0; i < (long)t.iterations; i++) { /* tight loop */ }

        t.end_tick    = NOW();
        self->has_task = 0;   /* free the worker */

        LOCK(s->metric_lock);
            if (s->metric_count < METRICS_CAP) {
                Metric *m           = &s->metrics[s->metric_count++];
                m->task_id          = t.id;
                m->type             = TYPE_MONSTER;
                m->weight           = t.weight;
                m->wait_ticks       = t.start_tick - t.arrival_tick;
                m->exec_ticks       = t.end_tick   - t.start_tick;
                m->turnaround_ticks = t.end_tick   - t.arrival_tick;
                m->cold_ticks       = 0;   /* workers never cold-start */
            }
        UNLOCK(s->metric_lock);
    }
    return (void *)0;
}

/* ================================================================== */
/* 10. DYNAMIC LOAD BALANCER DRIVER                                    */
/*                                                                     */
/* Fires every DLB_TICKS ticks independently of task completion.      */
/* Checks each worker's load ratio. If a worker exceeds              */
/* 75% load, notionally redistributes load to the least-loaded one.  */
/* ================================================================== */

static int worker_load(Worker *w) {
    if (!w->has_task) return 0;
    /* capacity = 10000 always, so load equals task weight directly */
    return (w->current_task.weight * 10000) / w->capacity;
}

void do_rebalance(Scheduler *s) {
    int i, j, li, lj, min_load, min_j;

    for (i = 0; i < NUM_WORKERS; i++) {
        li = worker_load(&s->workers[i]);
        if (li <= 7500) continue;   /* 0.75 scaled */

        min_j    = -1;
        min_load = li;

        for (j = 0; j < NUM_WORKERS; j++) {
            if (j == i) continue;
            lj = worker_load(&s->workers[j]);
            if (lj < min_load) {
                min_load = lj;
                min_j    = j;
            }
        }

        /*
         * In a real distributed system this would physically migrate
         * a sub-task. Here we record the event for metric purposes.
         */
        if (min_j >= 0) {
            /* migration modelled — no actual task movement in simulation */
            (void)min_j;
        }
    }
}

void *dlb_driver(void *arg) {
    Scheduler *s = (Scheduler *)arg;
    while (1) {
        SLEEP(DLB_TICKS);
        if (!sched_alive(s)) return (void *)0;
        do_rebalance(s);
    }
    return (void *)0;
}

/* ================================================================== */
/* 11. ARRIVAL DRIVER                                                  */
/*                                                                     */
/* Pulls task batches from task_source and calls submit() to enqueue  */
/* them. Sleeps between batches during the trickle phase.             */
/* Completely agnostic to total task count or arrival pattern.        */
/* ================================================================== */

void *arrival_driver(void *arg) {
    ArrivalArg        *aa  = (ArrivalArg *)arg;
    Scheduler         *s   = aa->s;
    struct TaskSource *src = aa->src;
    Task               buf[500];
    int                n;

    while (src_has_more(src)) {
        n = src_fetch(src, buf, 500);
        if (n > 0) submit(s, buf, n);
        if (src_streaming(src)) SLEEP(5);   /* 5 ticks ≈ 50ms */
    }
    return (void *)0;
}

/* ================================================================== */
/* 12. SHUTDOWN CONTROLLER                                             */
/*                                                                     */
/* The only component that sets running = 0.                          */
/* Polls until both conditions are simultaneously true:               */
/*   a) Both queues are empty                                         */
/*   b) All workers and FaaS threads are idle                         */
/*                                                                     */
/* Checking both together prevents premature shutdown during stream   */
/* gaps where a queue temporarily empties between arrival batches.    */
/* ================================================================== */

void *shutdown_driver(void *arg) {
    Scheduler *s = (Scheduler *)arg;
    int i, aq, mq, wi, fi;

    while (1) {
        SLEEP(POLL_TICKS);

        LOCK(s->ant_lock);
            aq = heap_empty(&s->ant_q);
        UNLOCK(s->ant_lock);

        LOCK(s->monster_lock);
            mq = heap_empty(&s->monster_q);
        UNLOCK(s->monster_lock);

        wi = 1;
        for (i = 0; i < NUM_WORKERS; i++)
            if (s->workers[i].has_task) { wi = 0; break; }

        LOCK(s->faas_lock);
            fi = 1;
            for (i = 0; i < FAAS_POOL_SIZE; i++)
                if (s->faas[i].state == STATE_BUSY) { fi = 0; break; }
        UNLOCK(s->faas_lock);

        /* All queues drained and all threads idle — safe to stop */
        if (aq && mq && wi && fi) {
            LOCK(s->run_lock);
                s->running = 0;
            UNLOCK(s->run_lock);
            return (void *)0;
        }
    }
    return (void *)0;
}

/* ================================================================== */
/* 13. SCHED_ALIVE                                                     */
/* Thread-safe read of the running flag.                               */
/* ================================================================== */

int sched_alive(Scheduler *s) {
    int r;
    LOCK(s->run_lock);
        r = s->running;
    UNLOCK(s->run_lock);
    return r;
}
