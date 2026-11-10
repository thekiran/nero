/* Run-time animation for the -Syn engine.
 *
 * The renderer owns no engine state: it borrows a view for the lifetime of the
 * thread and publishes the cursor row so the engine's interrupt handler can
 * clear the same screen area. */
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "nero/ascii.h"
#include "nero/terminal.h"

#include "syn_internal.h"

extern pthread_mutex_t ekran_mutex;

void *nero_syn_animation_main(void *arg)
{
    const nero_syn_animation_view *view =
        (const nero_syn_animation_view *)arg;
    int frame_count = 4;
    struct timespec delay = {0, 600000000};
    const char *dots[] = {".  ", ".. ", "...", "...."};

    while (!*view->stop_flag) {
        for (int i = 0; i < frame_count; i++) {
            if (*view->stop_flag)
                break;

            pthread_mutex_lock(&ekran_mutex);
            *view->row = row_location_function();

            printf("\033[%d;1H", (int)*view->row);
            printf("\033[J");

            printf("\033[40m\033[1;31m[*] \033[1;34mSYN Flood "
                   "\033[0m"
                   "\033[40m\033[1;33m%s \033[1;37mon "
                   "\033[40m\033[1;33m%d \033[1;37m"
                   "\033[1;32m%ld \033[1;37mthread%s\033[0m\n\n",
                   view->target_ip, view->port, view->thread_count, dots[i]);

            fflush(stdout);

            printf("%s", windows[i]);
            fflush(stdout);
            nanosleep(&delay, NULL);

            pthread_mutex_unlock(&ekran_mutex);
        }
    }

    return NULL;
}
