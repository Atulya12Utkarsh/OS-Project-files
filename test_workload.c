#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// Explicitly declare sleep to satisfy the strict compiler
int sleep(int);

// Custom delay function
void spin_delay(int delay_ticks) {
    int start = uptime();
    while (uptime() - start < delay_ticks) {
        // Busy-wait
    }
}

void do_work(int is_monster) {
    int limit;
    int primes = 0;
    int i, j, is_prime;

    limit = is_monster ? 1000000 : 1000;
    
    for (i = 2; i < limit; i++) {
        is_prime = 1;
        for (j = 2; j * j <= i; j++) {
            if (i % j == 0) {
                is_prime = 0;
                break;
            }
        }
        if (is_prime) {
            primes++;
        }
    }
    exit(0); 
}

int main(void) {
    int start_time, end_time, total_ticks;
    int pid, i, is_monster;
    
    // ---> NEW VARIABLES FOR METRICS <---
    int wtime, rtime;
    int total_wtime = 0;
    int total_rtime = 0;
    
    start_time = uptime();
    
    printf("--- Starting Bimodal Workload Test ---\n");

    // Phase 1: The Flood
    printf("Phase 1: Spawning 25 tasks (Flood)...\n");
    for (i = 0; i < 25; i++) {
        is_monster = (i % 5 == 0);
        pid = fork();
        if (pid == 0) {
            do_work(is_monster);
        }
    }

    // Phase 2: The Trickle
    printf("Phase 2: Spawning 15 tasks (Trickle)...\n");
    for (i = 25; i < 40; i++) {
        is_monster = (i % 5 == 0);
        pid = fork();
        if (pid == 0) {
            do_work(is_monster);
        }
        spin_delay(5); 
    }

    spin_delay(50); 

    // Phase 3: The Burst
    printf("Phase 3: Spawning 10 tasks (Burst)...\n");
    for (i = 40; i < 50; i++) {
        is_monster = (i % 5 == 0);
        pid = fork();
        if (pid == 0) {
            do_work(is_monster);
        }
    }

    // ---> NEW WAIT LOOP <---
    // Wait for all 50 tasks using our custom waitx system call!
    for (i = 0; i < 50; i++) {
        waitx(&wtime, &rtime); 
        total_wtime += wtime;
        total_rtime += rtime;
    }

    end_time = uptime();
    total_ticks = end_time - start_time;

    printf("\n--- Test Complete ---\n");
    printf("Total Turnaround Time: %d ticks\n", total_ticks);
    printf("Throughput: 50 tasks in %d ticks\n", total_ticks);
    
    // ---> PRINT THE NEW METRICS <---
    printf("Total Wait Time: %d ticks\n", total_wtime);
    printf("Total CPU Utilization: %d ticks\n", total_rtime);

    exit(0);
}
