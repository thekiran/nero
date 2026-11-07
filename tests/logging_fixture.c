#include "nero/log.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define make_directory(p) _mkdir(p)
#define remove_directory(p) _rmdir(p)
#else
#include <unistd.h>
#define make_directory(p) mkdir(p,0700)
#define remove_directory(p) rmdir(p)
#endif

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
    if (argc>1 && strcmp(argv[1],"first-error")==0) {
        nero_log_write(NERO_LOG_ERROR,"fixture","FIRST_ERROR_AFTER_PARTIAL_LINE");
    } else if (argc>1 && (strcmp(argv[1],"clear-after-failure")==0 || strcmp(argv[1],"clear-failure")==0)) {
        const char *path=getenv("NERO_LOG_FILE");
        char saved[2048];
        if (!path) return 3;
        snprintf(saved,sizeof(saved),"%s.saved",path);
        nero_log_write(NERO_LOG_ERROR,"fixture","OLD_PERSISTED_ERROR");
        if (rename(path,saved)!=0 || make_directory(path)!=0) return 4;
        nero_log_write(NERO_LOG_ERROR,"fixture","TEMPORARY_WRITE_FAILURE");
        if (strcmp(argv[1],"clear-after-failure")==0 &&
            (remove_directory(path)!=0 || rename(saved,path)!=0)) return 5;
        printf("clear_result=%d\n",nero_log_clear());
    } else if (argc>1 && strcmp(argv[1],"stress")==0) {
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
