/* ACM MPEG Layer II -> PCM16 streams against a synthetic 440 Hz, 22.05 kHz mono clip. */
#include "acm-api.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
uint8_t *g_mem;
void guest_trap(CPU *c,uint32_t a,const char *m) { (void)c;(void)a;fprintf(stderr,"trap %s\n",m);abort(); }
static uint32_t call(const char *name,unsigned n,const uint32_t *args) {
    CPU c={0};c.esp=0x100;W32(c.esp,0xdeadbeef);for(unsigned i=0;i<n;i++)W32(c.esp+4+4*i,args[i]);
    uint32_t count;ImportHandler *h=movies_acm_handlers(&count);
    for(unsigned i=0;i<count;i++)if(!strcmp(strchr(h[i].name,'!')+1,name)) { h[i].fn(&c);assert(c.esp==0x104+4*n);return c.eax; }
    assert(0);return 0;
}
int main(void) {
    g_mem=calloc(1,0x100000);assert(g_mem);
    FILE *f=fopen(FIXTURE,"rb");assert(f);uint32_t src=0x10000;size_t n=fread(HP(src),1,0x10000,f);fclose(f);assert(n>4000);
    /* MPEG1WAVEFORMAT-style source (tag 0x50) and PCM destination. */
    W16(0x1000,0x50);W16(0x1002,1);W32(0x1004,22050);W32(0x1008,8000);W16(0x100c,1);W16(0x100e,0);W16(0x1010,22);
    W16(0x1100,1);W16(0x1102,1);W32(0x1104,22050);W32(0x1108,44100);W16(0x110c,2);W16(0x110e,16);W16(0x1110,0);
    assert(call("acmStreamOpen",8,(uint32_t[]){0x1200,0,0x1000,0x1100,0,0,0,1})==0); /* query */
    W16(0x1102,2);assert(call("acmStreamOpen",8,(uint32_t[]){0x1200,0,0x1000,0x1100,0,0,0,4})==512);W16(0x1102,1);
    assert(call("acmStreamOpen",8,(uint32_t[]){0x1200,0,0x1000,0x1100,0,0,0,4})==0);uint32_t has=R32(0x1200);assert(has);
    assert(call("acmStreamSize",4,(uint32_t[]){has,(uint32_t)n,0x1204,0})==0);uint32_t out_size=R32(0x1204);assert(out_size>=23040*2);
    assert(call("acmStreamSize",4,(uint32_t[]){has,8192,0x1208,1})==0 && R32(0x1208)>0);
    /* Convert in two uneven source chunks into a large destination. */
    uint32_t hdr=0x2000,dst=0x40000,total=0;memset(HP(hdr),0,0x54);W32(hdr,0x54);
    uint32_t chunks[2]={3001,(uint32_t)n-3001},at=0;
    for(int i=0;i<2;i++) {
        W32(hdr+12,src+at);W32(hdr+16,chunks[i]);W32(hdr+28,dst+total);W32(hdr+32,0x80000);
        assert(call("acmStreamConvert",3,(uint32_t[]){has,hdr,0})==514); /* unprepared */
        assert(call("acmStreamPrepareHeader",3,(uint32_t[]){has,hdr,0})==0 && (R32(hdr+4)&0x20000));
        assert(call("acmStreamConvert",3,(uint32_t[]){has,hdr,i?0x20:0x14})==0 && (R32(hdr+4)&0x10000));
        assert(R32(hdr+20)==chunks[i]);total+=R32(hdr+36);at+=chunks[i];
        assert(call("acmStreamUnprepareHeader",3,(uint32_t[]){has,hdr,0})==0);
    }
    assert(total==23040*2);
    /* 440 Hz over 1.045 s: count zero crossings in the steady part, check level. */
    unsigned crossings=0;double energy=0;
    for(unsigned i=2000;i<22000;i++) { int16_t a=(int16_t)R16(dst+2*i),b=(int16_t)R16(dst+2*i+2);if((a<0)!=(b<0))crossings++;energy+=(double)a*a; }
    double seconds=20000.0/22050,rms=sqrt(energy/20000);
    assert(fabs(crossings/(2*seconds)-440)<5 && rms>1000);
    /* Small destination: output stays pending and drains on the next call. */
    assert(call("acmStreamReset",2,(uint32_t[]){has,0})==0);
    W32(hdr+4,0x20000);W32(hdr+12,src);W32(hdr+16,(uint32_t)n);W32(hdr+28,dst);W32(hdr+32,1000);
    assert(call("acmStreamConvert",3,(uint32_t[]){has,hdr,0x10})==0 && R32(hdr+36)==1000 && R32(hdr+20)==n);
    W32(hdr+16,0);W32(hdr+32,0x80000);assert(call("acmStreamConvert",3,(uint32_t[]){has,hdr,0})==0 && R32(hdr+36)==23040*2-1000);
    assert(call("acmStreamClose",2,(uint32_t[]){has,0})==0 && call("acmStreamClose",2,(uint32_t[]){has,0})==5);
    free(g_mem);puts("ACM MPEG Layer II conversion tests passed");
}
