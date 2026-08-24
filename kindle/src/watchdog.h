#ifndef AIQUOTA_WATCHDOG_H
#define AIQUOTA_WATCHDOG_H

/* Liveness heartbeat for extension/bin/watchdog.sh.
 *
 * The shell watchdog kill -9s this process when the heartbeat file stops
 * being touched, which is what lets a frozen Kindle framework recover from a
 * wedged app (the wrapper's EXIT trap runs as soon as we die). For that to
 * detect a wedge and not just a dead process, the beat must WITNESS the main
 * thread: the beat thread only touches the file while the main thread is
 * inside a window it announced with wd_expect(). A main thread stuck in a
 * loop -- or blocked past its own timeout -- stops the beat even though the
 * beat thread itself is perfectly healthy.
 *
 * Every blocking call on the main path therefore announces its own bound
 * (see http.c, net.c, input.c callers in main.c). There is deliberately no
 * "wait forever" state: an un-announced overrun stops the beat and gets the
 * app killed, which restores the framework -- the safe direction to fail. */

/* Grace added on top of a phase's own timeout, to cover the syscall
 * returning slightly late plus one whole beat interval. */
#define WD_SLACK_S 15

/* Bound for local work with no timeout of its own: render, refresh, parse. */
#define WD_STEP_S 60

/* Open ping_path and start the detached beat thread. Failure is silent and
 * non-fatal: the shell watchdog then simply never sees a heartbeat file and
 * falls back to its other duties. Call once. */
void wd_start(const char *ping_path);

/* Main thread: "I will call wd_expect again within secs seconds." */
void wd_expect(int secs);

/* Whether the beat thread may touch the file at time `now` (epoch seconds).
 * Exposed for unit tests; the beat thread calls it with time(NULL). */
int wd_should_beat(long long now);

#endif
