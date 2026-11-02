#include <pthread.h>
#include <stdio.h>

#include "nero/ascii.h"
#include "nero/terminal.h"

#define COLOR "\033[35m\n" // Magenta banner color
#define CLEAR "\033[0m\n"  // Reset color

pthread_mutex_t ekran_mutex = PTHREAD_MUTEX_INITIALIZER;

int ascii_function(void)
{
    pthread_mutex_lock(&ekran_mutex);
    clear_screen();
    if (!terminal_unicode_enabled()) {
        puts("\nN E R O\n");
        fflush(stdout);
        pthread_mutex_unlock(&ekran_mutex);
        return 0;
    }
    fputs(terminal_color_enabled() ? COLOR : "\n", stdout);
    printf("                                                         \n"
           "                                                         \n"
           "░▒▓███████▓▒░ ░▒▓████████▓▒░░▒▓███████▓▒░  ░▒▓██████▓▒░  \n"
           "░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░       ░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░ \n"
           "░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░       ░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░ \n"
           "░▒▓█▓▒░░▒▓█▓▒░░▒▓██████▓▒░  ░▒▓███████▓▒░ ░▒▓█▓▒░░▒▓█▓▒░ \n"
           "░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░       ░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░ \n"
           "░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░       ░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░ \n"
           "░▒▓█▓▒░░▒▓█▓▒░░▒▓████████▓▒░░▒▓█▓▒░░▒▓█▓▒░ ░▒▓██████▓▒░  \n"
           "                                                         \n");
    fputs(terminal_color_enabled() ? CLEAR : "\n", stdout);
    fflush(stdout);
    pthread_mutex_unlock(&ekran_mutex);

    return 0;
}

/* Legacy animation frames consumed by the Linux module. */

const char *windows[] = {
    // Frame 1
    "     \033[41m         \033[0m                    \033[42m         \033[0m\n"
    "     \033[41m  NERO  \033[0m\033[43mSYN\033[0m                   \033[42m TARGET \033[0m\n"
    "     \033[41m         \033[0m                    \033[42m         \033[0m\n",

    // Frame 2
    "     \033[41m         \033[0m                    \033[42m         \033[0m\n"
    "     \033[41m  NERO  \033[0m     \033[43mSYN\033[0m              \033[42m TARGET \033[0m\n"
    "     \033[41m         \033[0m                    \033[42m         \033[0m\n",

    // Frame 3
    "     \033[41m         \033[0m                    \033[42m         \033[0m\n"
    "     \033[41m  NERO  \033[0m           \033[43mSYN\033[0m        \033[42m TARGET \033[0m\n"
    "     \033[41m         \033[0m                    \033[42m         \033[0m\n",

    // Frame 4
    "     \033[41m         \033[0m                    \033[42m         \033[0m\n"
    "     \033[41m  NERO  \033[0m                   \033[43mSYN\033[0m\033[42m TARGET \033[0m\n"
    "     \033[41m         \033[0m                    \033[42m         \033[0m\n"};
