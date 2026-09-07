#ifndef AIQUOTA_MONO_H
#define AIQUOTA_MONO_H

/* Monotonic seconds for duration deadlines (R09). Falls back to time(NULL)
 * when CLOCK_MONOTONIC is unavailable. Display / watchdog heartbeat mtimes
 * keep using the wall clock so shell `date` comparisons stay valid. */
long mono_now_s(void);
long mono_now_ms(void);

#endif
