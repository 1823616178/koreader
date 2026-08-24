/* Heartbeat thread + main-thread deadline. See watchdog.h for the contract
 * and extension/bin/watchdog.sh for the consumer. */
#define _POSIX_C_SOURCE 200809L /* pwrite */

#include "watchdog.h"

#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

/* One beat every 30 s against watchdog.sh's 120 s staleness threshold: four
 * beats of margin, so a single delayed beat can never look like a wedge.
 * Each beat is a CPU wake from suspend; 30 s halves that churn vs 15 s and
 * still catches a wedged main thread well inside a minute of it wedging
 * (detection latency is bounded by watchdog.sh's two-confirm rule). */
#define WD_BEAT_S 30

static pthread_mutex_t wd_lock = PTHREAD_MUTEX_INITIALIZER;
/* Epoch second the main thread promised to be back by; 0 = not armed yet
 * (between wd_start and the first wd_expect), which beats unconditionally. */
static time_t wd_deadline;

void wd_expect(int secs)
{
    time_t deadline;

    if (secs < 1)
        secs = 1;
    deadline = time(NULL) + secs;

    pthread_mutex_lock(&wd_lock);
    wd_deadline = deadline;
    pthread_mutex_unlock(&wd_lock);
}

int wd_should_beat(long long now)
{
    time_t deadline;

    pthread_mutex_lock(&wd_lock);
    deadline = wd_deadline;
    pthread_mutex_unlock(&wd_lock);

    return deadline == 0 || (long long)deadline >= now;
}

/* Rewrites the same byte at offset 0 rather than appending: the shell side
 * only reads mtime, and write(2) bumps mtime whatever the content is. */
static void *wd_beat_main(void *arg)
{
    int fd = *(const int *)arg;

    for (;;) {
        if (wd_should_beat((long long)time(NULL))) {
            if (pwrite(fd, ".", 1, 0) != 1)
                break; /* /var/tmp went away: nothing more we can do */
        }
        /* Overdue: skip the beat but keep looping, so a main thread that is
         * merely slow (not wedged) resumes beating and survives. */
        sleep(WD_BEAT_S);
    }
    return NULL;
}

void wd_start(const char *ping_path)
{
    static int ping_fd; /* static: the thread reads it after we return */
    pthread_t tid;

    ping_fd = open(ping_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ping_fd < 0)
        return;
    if (pthread_create(&tid, NULL, wd_beat_main, &ping_fd) != 0) {
        close(ping_fd);
        return;
    }
    pthread_detach(tid);
}
