#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>

#include "terminal.h"
#include "syn_flood_powered.h"
#include "ascii.h"

#define COLOR "\033[35m\n"  // Siyah arka plan, beyaz yazı
#define CLEAR "\033[0m\n"      // Renk sıfırlama

extern const char *windows[];

int row_location_function();

pthread_mutex_t ekran_mutex = PTHREAD_MUTEX_INITIALIZER;

int ascii_function() {
    printf("\033[H\033[J");
    system("clear");
    fflush(stdout);
    printf(COLOR
           "                                                         \n"
           "                                                         \n"
           "░▒▓███████▓▒░ ░▒▓████████▓▒░░▒▓███████▓▒░  ░▒▓██████▓▒░  \n"
           "░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░       ░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░ \n"
           "░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░       ░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░ \n"
           "░▒▓█▓▒░░▒▓█▓▒░░▒▓██████▓▒░  ░▒▓███████▓▒░ ░▒▓█▓▒░░▒▓█▓▒░ \n"
           "░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░       ░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░ \n"
           "░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░       ░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░ \n"
           "░▒▓█▓▒░░▒▓█▓▒░░▒▓████████▓▒░░▒▓█▓▒░░▒▓█▓▒░ ░▒▓██████▓▒░  \n"
           "                                                         \n"
           CLEAR);fflush(stdout); 

    return 0;
}


// Animasyonu gösteren thread fonksiyonu

const char *windows[] = 
{
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
    "     \033[41m         \033[0m                    \033[42m         \033[0m\n"
};

