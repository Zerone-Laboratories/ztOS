/* tickle: pause apps that are not in front, in short pulses (zrnperfd). */
#pragma once
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>

/* the laptop zrn_perfd's four modes */
enum tickle_mode { TICKLE_NONE, TICKLE_NOMINAL, TICKLE_MODERATE, TICKLE_HIGH };
extern const char *const tickle_names[4];

void tickle_init(void);
void tickle_stop(void);				/* every app running again */
void tickle_set_mode(enum tickle_mode mode);
void tickle_focus(pid_t pid, const char *app_id);	/* pid 0: Start is in front */
void tickle_screen(bool on);
int tickle_pollfds(struct pollfd *fds, int max);	/* what to watch */
void tickle_ready(const struct pollfd *fds, int n);	/* after poll() */
int tickle_timeout(void);			/* ms to the next pulse, -1 none */
bool tickle_run(void);				/* do what is due; true: the status changed */
void tickle_status(FILE *f);

void tickle_background_reload(void);
int tickle_background_count(void);
bool tickle_audio_playing(void);		/* a stream is playing, Android's included */
