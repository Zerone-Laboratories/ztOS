/* No modem or network access: exercise only the screen command's scheduling. */
#define main networking_daemon_main
#include "../ztoscorenetworkingd.c"
#undef main
#include <assert.h>
int main(void) {
    screen_off = true;
    silent_polls = 8;
    next_poll = ms() + 600000;
    long before = ms();
    char wake[] = "screen on";
    command(wake);
    assert(!screen_off);
    assert(silent_polls == 0);
    assert(next_poll >= before && next_poll <= ms());
    char sleep[] = "screen off";
    command(sleep);
    assert(screen_off);
    return 0;
}
