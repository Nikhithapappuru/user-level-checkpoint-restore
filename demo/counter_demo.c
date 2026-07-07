#define _GNU_SOURCE

#include "ulcr/runtime.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define DEMO_STATE_ADDR ((void *) 0x530000000000ULL)
#define DEMO_STATE_SIZE 4096

typedef struct demo_state {
    uint64_t initialized;
    uint64_t counter;
    uint64_t restored_runs;
    char note[128];
} demo_state_t;

static volatile sig_atomic_t keep_running = 1;

static void on_signal(int signo) {
    (void) signo;
    keep_running = 0;
}

static demo_state_t *map_demo_state(void) {
    void *mapped = mmap(
        DEMO_STATE_ADDR,
        DEMO_STATE_SIZE,
        PROT_READ | PROT_WRITE,
        MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0
    );
    if (mapped == MAP_FAILED) {
        return NULL;
    }
    return (demo_state_t *) mapped;
}

int main(int argc, char **argv) {
    demo_state_t *state = NULL;
    int restored = ulcr_runtime_bootstrap(argc, argv);

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    state = (demo_state_t *) DEMO_STATE_ADDR;
    if (restored == 0) {
        state = map_demo_state();
        if (state == NULL) {
            perror("mmap demo state");
            return 1;
        }
    }

    if (!state->initialized) {
        memset(state, 0, sizeof(*state));
        state->initialized = 1;
        snprintf(state->note, sizeof(state->note), "demo state mapped at fixed address");
    }
    if (restored > 0) {
        state->restored_runs += 1;
    }

    while (keep_running) {
        char timestamp[64];
        time_t now = time(NULL);
        struct tm tm_now;

        localtime_r(&now, &tm_now);
        strftime(timestamp, sizeof(timestamp), "%F %T", &tm_now);
        state->counter += 1;

        printf(
            "[counter-demo] pid=%d counter=%llu restored_runs=%llu at=%s note=\"%s\"\n",
            getpid(),
            (unsigned long long) state->counter,
            (unsigned long long) state->restored_runs,
            timestamp,
            state->note
        );
        fflush(stdout);
        sleep(1);
    }

    printf("[counter-demo] graceful shutdown pid=%d counter=%llu\n", getpid(), (unsigned long long) state->counter);
    fflush(stdout);
    return 0;
}
