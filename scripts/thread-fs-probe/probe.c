#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
static atomic_int phase,done,error;
static uint32_t digest;static size_t bytes;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond=PTHREAD_COND_INITIALIZER;
static void *reader(void *unused){(void)unused;atomic_store(&phase,1);
 FILE *file=fopen("/game/Data/Audio/Atmosphere/fac_catering0.lug","rb");
 if(!file){atomic_store(&error,1);}else{unsigned char buffer[128];size_t n;digest=2166136261u;while((n=fread(buffer,1,sizeof buffer,file))){for(size_t i=0;i<n;i++)digest=(digest^buffer[i])*16777619u;bytes+=n;}if(ferror(file))atomic_store(&error,2);if(fclose(file))atomic_store(&error,3);}
 pthread_mutex_lock(&mutex);atomic_store(&done,1);pthread_cond_signal(&cond);pthread_mutex_unlock(&mutex);return NULL;
}
int main(int argc,char **argv){int mode=argc>1?atoi(argv[1]):0;pthread_t child;double start=emscripten_get_now();if(pthread_create(&child,NULL,reader,NULL)){puts("RESULT {\"passed\":false,\"error\":\"pthread_create\"}");return 1;}
 /* Wait for child execution to begin without dispatching filesystem proxies. */
 while(!atomic_load(&phase)&&emscripten_get_now()-start<2000){}
 int started=atomic_load(&phase),blocked=0;unsigned pumps=0;
 if(mode==0){double until=emscripten_get_now()+350;while(emscripten_get_now()<until){}blocked=!atomic_load(&done);}
 double deadline=emscripten_get_now()+5000;
 while(!atomic_load(&done)&&emscripten_get_now()<deadline){
  if(mode==2){struct timespec timeout;clock_gettime(CLOCK_REALTIME,&timeout);timeout.tv_nsec+=5000000;if(timeout.tv_nsec>=1000000000){timeout.tv_nsec-=1000000000;timeout.tv_sec++;}pthread_mutex_lock(&mutex);if(!atomic_load(&done))pthread_cond_timedwait(&cond,&mutex,&timeout);pthread_mutex_unlock(&mutex);}
  else {emscripten_main_thread_process_queued_calls();pumps++;}
 }
 if(!atomic_load(&done)){printf("RESULT {\"passed\":false,\"mode\":%d,\"error\":\"child stalled\",\"started\":%d}\n",mode,started);return 2;}
 pthread_join(child,NULL);int pass=started&&!atomic_load(&error)&&bytes==EXPECTED_BYTES&&digest==EXPECTED_FNV&&(mode!=0||blocked);
 printf("RESULT {\"passed\":%s,\"mode\":%d,\"childStarted\":%d,\"blockedBeforePump\":%d,\"pumpCalls\":%u,\"bytes\":%zu,\"fnv\":%u,\"error\":%d,\"elapsedMs\":%.3f}\n",pass?"true":"false",mode,started,blocked,pumps,bytes,digest,atomic_load(&error),emscripten_get_now()-start);return pass?0:3;}
