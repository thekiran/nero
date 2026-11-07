#include "nero/log.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *writer(void *arg) {
    long id = *(long *)arg;
    for (int i=0;i<600;++i) {
        errno=EDOM;
        nero_log_write(i%3 == 0 ? NERO_LOG_ERROR : NERO_LOG_INFO,"worker",
                       "worker=%ld event=%d payload=%0600d",id,i,i);
        if (errno!=EDOM) abort();
    }
    return NULL;
}
int main(int argc, char **argv) {
    if (argc>1 && strcmp(argv[1],"stress")==0) {
        pthread_t threads[8]; long ids[8];
        for (long i=0;i<8;++i) {
            ids[i]=i;
            if (pthread_create(&threads[i],NULL,writer,&ids[i])!=0) return 1;
        }
        for (int i=0;i<8;++i) pthread_join(threads[i],NULL);
    } else {
        errno=EDOM;
        nero_log_write(NERO_LOG_INFO,"fixture","info marker");
        nero_log_write(NERO_LOG_WARN,"fixture","warning marker");
        nero_log_write(NERO_LOG_ERROR,"fixture","error marker\nESC=\033[31m end");
        if (errno!=EDOM) return 2;
    }
    nero_log_dump(stdout,argc>1 && strcmp(argv[1],"errors")==0);
    return 0;
}
