#define _DARWIN_C_SOURCE

#define _POSIX_C_SOURCE 200809L
#include "nero/tcp_capacity.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static int nonblock(int fd) { int f=fcntl(fd,F_GETFL); return f<0 ? -1 : fcntl(fd,F_SETFL,f|O_NONBLOCK); }
/* Counters are namespace-wide, never attributed to a remote target. */
static void observe(void) {
#ifdef __linux__
    FILE *f=fopen("/proc/net/netstat","r");
    char names[8192],values[8192];
    if(f) {
        while(fgets(names,sizeof names,f) && fgets(values,sizeof values,f)) {
            if(strncmp(names,"TcpExt:",7)) continue;
            char *a=NULL,*b=NULL,*n=strtok_r(names," \n",&a),*v=strtok_r(values," \n",&b);
            while(n && v) {
                if(!strcmp(n,"SyncookiesSent") || !strcmp(n,"SyncookiesRecv") || !strcmp(n,"SyncookiesFailed") || !strcmp(n,"ListenOverflows") || !strcmp(n,"ListenDrops")) printf("%s=%s ",n,v);
                n=strtok_r(NULL," \n",&a); v=strtok_r(NULL," \n",&b);
            }
            puts("");
        }
        fclose(f);
    } else puts("BLOCKED: TCP counters unavailable");
    const char *paths[]={"/proc/net/tcp","/proc/net/tcp6"};
    for(int i=0;i<2;i++) {
        f=fopen(paths[i],"r"); unsigned count=0,state; char line[1024];
        if(!f) { printf("BLOCKED: %s unavailable\n",paths[i]); continue; }
        while(fgets(line,sizeof line,f)) if(sscanf(line," %*d: %*s %*s %x",&state)==1 && state==3) count++;
        fclose(f); printf("%s SYN_RECV=%u (namespace-wide)\n",paths[i],count);
    }
#else
    puts("BLOCKED: portable macOS adapter has no reliable SYN queue/cookie counter source");
#endif
}
int nero_tcp_test(char **argv,size_t argc,volatile sig_atomic_t *cancel) {
    int port=80,concurrency=32,rate=50,duration=5,timeout=1000,ramp=2,hold=100,fixture=0,backlog=16;
    int authorized=0,observation=0;
    struct sockaddr_in address={0}; address.sin_family=AF_INET;
    if(inet_pton(AF_INET,argv[0],&address.sin_addr)!=1) return -1;
    for(size_t i=2;i<argc;i++) {
        if(!strcmp(argv[i],"--fixture")) {fixture=1;continue;}
        if(!strcmp(argv[i],"--authorized-lab")) {authorized=1;continue;}
        if(i+1==argc) return -1;
        const char *key=argv[i++],*value=argv[i];
        if(!strcmp(key,"--mode")) {
            if(strcmp(value,"connections") && strcmp(value,"backlog")) return -1;
            observation=!strcmp(value,"backlog");continue;
        }
        char *end; errno=0; long n=strtol(value,&end,10);
        if(errno || *end || !*value || n<0 || n>65535) return -1;
        if(!strcmp(key,"--port")) port=(int)n;
        else if(!strcmp(key,"--concurrency")) concurrency=(int)n;
        else if(!strcmp(key,"--rate")) rate=(int)n;
        else if(!strcmp(key,"--duration")) duration=(int)n;
        else if(!strcmp(key,"--timeout-ms")) timeout=(int)n;
        else if(!strcmp(key,"--ramp")) ramp=(int)n;
        else if(!strcmp(key,"--hold-ms")) hold=(int)n;
        else if(!strcmp(key,"--listen-backlog")) backlog=(int)n;
        else return -1;
    }
    int local=(ntohl(address.sin_addr.s_addr)>>24)==127;
    if(!port || concurrency<1 || concurrency>256 || rate<1 || rate>200 || duration<1 || duration>60 || timeout<1 || timeout>10000 || ramp>duration || hold>10000 || backlog<1 || backlog>128) return -1;
    if((!local && (!authorized || observation || fixture)) || (observation && fixture)) {
        puts("BLOCKED: active remote tests require --authorized-lab; observations and fixtures are local only"); return 1;
    }
    puts("Local kernel snapshot BEFORE (remote target counters are not accessible):"); observe();
    puts("SYN backlog holds incomplete handshakes; the accept queue holds completed connections. ListenOverflows/ListenDrops alone do not establish SYN backlog exhaustion.");
    double start=now(),next=start;
    if(observation) {
        while(!*cancel && now()-start<duration) {poll(NULL,0,1000); observe();}
        puts("BLOCKED: SYN backlog exhaustion cannot be established from these observations"); return 0;
    }
    int listener=-1;
    if(fixture) {
        listener=socket(AF_INET,SOCK_STREAM,0); address.sin_port=0;
        if(listener<0 || nonblock(listener)<0 || bind(listener,(struct sockaddr *)&address,sizeof address)<0 || listen(listener,backlog)<0) {if(listener>=0)close(listener);perror("fixture");return 1;}
        socklen_t len=sizeof address;
        if(getsockname(listener,(struct sockaddr *)&address,&len)<0) {close(listener);return 1;}
        printf("Fixture port=%u accept backlog=%d (continuously drained)\n",ntohs(address.sin_port),backlog);
    } else address.sin_port=htons((unsigned short)port);
    struct pollfd p[256]; double begun[256],done[256];
    for(int i=0;i<256;i++) {p[i].fd=-1;p[i].events=POLLOUT;done[i]=0;}
    unsigned attempts=0,success=0,failed=0,timedout=0,cancelled=0,peak=0; double latency=0,maxlat=0;
    struct rusage before,after; getrusage(RUSAGE_SELF,&before);
    while(!*cancel) {
        double t=now(); int active=0;
        for(int i=0;i<concurrency;i++) if(p[i].fd>=0) active++;
        if(t-start>=duration && !active) break;
        int limit=ramp ? 1+(int)((concurrency-1)*(t-start)/ramp) : concurrency;
        if(limit>concurrency)limit=concurrency;
        if(t-start<duration && active<limit && t>=next) {
            next=t+1.0/rate;
            for(int i=0;i<concurrency;i++) if(p[i].fd<0) {
                attempts++; int fd=socket(AF_INET,SOCK_STREAM,0);
                if(fd<0 || nonblock(fd)<0) {if(fd>=0)close(fd);failed++;break;}
                begun[i]=t; done[i]=0;
                int rc=connect(fd,(struct sockaddr *)&address,sizeof address);
                if(rc<0 && errno!=EINPROGRESS) {close(fd);failed++;break;}
                p[i].fd=fd;p[i].events=POLLOUT;active++;break;
            }
        }
        if((unsigned)active>peak)peak=(unsigned)active;
        if(poll(p,(nfds_t)concurrency,5)<0 && errno!=EINTR) {perror("poll");break;}
        if(listener>=0) {int fd;while((fd=accept(listener,NULL,NULL))>=0) close(fd);}
        t=now();
        for(int i=0;i<concurrency;i++) if(p[i].fd>=0) {
            int finish=0;
            if(done[i]) finish=t-done[i]>=hold/1000.0;
            else if(p[i].revents) {
                int error=0; socklen_t len=sizeof error;
                if(getsockopt(p[i].fd,SOL_SOCKET,SO_ERROR,&error,&len)<0 || error) {failed++;finish=1;}
                else {double ms=(t-begun[i])*1000;success++;latency+=ms;if(ms>maxlat)maxlat=ms;done[i]=t;p[i].events=0;}
            } else if(t-begun[i]>=timeout/1000.0) {failed++;timedout++;finish=1;}
            if(finish) {close(p[i].fd);p[i].fd=-1;}
        }
    }
    for(int i=0;i<concurrency;i++) if(p[i].fd>=0) {if(!done[i])cancelled++;close(p[i].fd);}
    if(listener>=0)close(listener);
    getrusage(RUSAGE_SELF,&after); double elapsed=now()-start;
    double cpu=(after.ru_utime.tv_sec-before.ru_utime.tv_sec)+(after.ru_utime.tv_usec-before.ru_utime.tv_usec)/1e6+(after.ru_stime.tv_sec-before.ru_stime.tv_sec)+(after.ru_stime.tv_usec-before.ru_stime.tv_usec)/1e6;
    printf("%s attempts=%u success=%u failures=%u timeouts=%u cancelled=%u success_rate=%.2f%% connections/s=%.2f latency_mean_ms=%.3f latency_max_ms=%.3f peak_sockets=%u cpu_seconds=%.4f maxrss_native=%ld elapsed=%.3f\n",*cancel?"BLOCKED":failed?"FAIL":"PASS",attempts,success,failed,timedout,cancelled,attempts?100.0*success/attempts:0,success/elapsed,success?latency/success:0,maxlat,peak,cpu,after.ru_maxrss,elapsed);
    puts("Local kernel snapshot AFTER:");observe();
    puts("BLOCKED: SYN backlog exhaustion unproven; throughput measures connections, not payload bytes. RSS is process high-water (bytes macOS, KiB Linux).");
    return failed || cancelled ? 1:0;
}
