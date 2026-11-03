#include "nero/tcp_capacity.h"
#include <fcntl.h>
#include <stdio.h>
static int descriptors(void) { int n=0; for(int i=0;i<1024;i++) if(fcntl(i,F_GETFD)>=0)n++;return n; }
int main(void) {
 volatile sig_atomic_t cancel=0;
 char *args[]={"127.0.0.1","-Syn","--mode","connections","--fixture","--duration","1","--ramp","0"};
 int before=descriptors();
 for(int i=0;i<3;i++) if(nero_tcp_test(args,9,&cancel))return 1;
 if(descriptors()!=before)return 2;
 args[0]="192.0.2.1";
 if(nero_tcp_test(args,9,&cancel)!=1)return 3;
 puts("PASS fixture repetitions, authorization, descriptor cleanup"); return 0;
}
