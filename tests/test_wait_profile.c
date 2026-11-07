#include "wait-profile.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
static MoviesWaitProfileReport reports[8];static unsigned count;
static void sink(const MoviesWaitProfileReport*r){assert(count<8);reports[count++]=*r;}
static void *child(void*p){(void)p;movies_wait_profile_interval(2,0xabcd,0xffffffffu,0,100,350);movies_wait_profile_present();return NULL;}
int main(void){
 movies_wait_profile_init(0,sink);assert(!movies_wait_profile_active());assert(!movies_wait_profile_begin());movies_wait_profile_interval(1,1,1,0,1,20);movies_wait_profile_present();movies_wait_profile_flush();assert(!count);
 movies_wait_profile_init(1,sink);assert(movies_wait_profile_begin());movies_wait_profile_interval(1,0x1111,10,0,100,999);movies_wait_profile_present();
 movies_wait_profile_interval(1,0x1234,10,0,100,200);movies_wait_profile_interval(1,0x1234,10,0,200,500);
 pthread_t thread;assert(!pthread_create(&thread,NULL,child,NULL));assert(!pthread_join(thread,NULL));
 for(unsigned i=0;i<119;i++)movies_wait_profile_present();assert(!count);movies_wait_profile_present();assert(count==1);
 assert(reports[0].frames==120&&reports[0].last_serial==121);assert(reports[0].entries[0][0].count==2&&reports[0].entries[0][0].ns==400&&reports[0].entries[0][0].max_ns==300);assert(reports[0].entries[1][0].count==1&&reports[0].entries[1][0].ns==250);
 for(unsigned i=0;i<40;i++)movies_wait_profile_interval(1,0x2000+i,10,0,1,2);movies_wait_profile_flush();assert(count==2);assert(reports[1].entries[0][16].kind==0&&reports[1].entries[0][16].count==24);
 movies_wait_profile_flush();assert(count==2);movies_wait_profile_interval(1,1,0,0,20,10);movies_wait_profile_flush();assert(count==2);
 puts("wait profiling bounded/reset/main-child tests passed");return 0;
}
