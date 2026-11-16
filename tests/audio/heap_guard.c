/* Host-only allocation instrumentation for the minimal FFmpeg fixture tests. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#define LIMIT (8U * 1024U * 1024U)
static struct { void *pointer; size_t size; } records[8192];
static size_t used,peak;
void *__real_malloc(size_t);
void *__real_calloc(size_t,size_t);
void *__real_realloc(void *,size_t);
void __real_free(void *);
int __real_posix_memalign(void **,size_t,size_t);
void *__real_aligned_alloc(size_t,size_t);
static size_t remove_record(void *p) {
    if(!p)return 0;
    for(unsigned i=0;i<8192;++i)if(records[i].pointer==p){size_t n=records[i].size;records[i].pointer=NULL;used-=n;return n;}
    return 0;
}
static void *record(void *p,size_t n) {
    if(!p)return p;
    for(unsigned i=0;i<8192;++i)if(!records[i].pointer){records[i].pointer=p;records[i].size=n;used+=n;if(used>peak)peak=used;return p;}
    abort();
}
void *__wrap_malloc(size_t n){return n>LIMIT-used?NULL:record(__real_malloc(n),n);}
void *__wrap_calloc(size_t n,size_t s){if(s&&n>SIZE_MAX/s)return NULL;size_t bytes=n*s;return bytes>LIMIT-used?NULL:record(__real_calloc(n,s),bytes);}
void __wrap_free(void *p){remove_record(p);__real_free(p);}
void *__wrap_realloc(void *p,size_t n){
    size_t previous=0;
    if(p)for(unsigned i=0;i<8192;++i)if(records[i].pointer==p){previous=records[i].size;break;}
    if(n>LIMIT-used+previous)return NULL;
    void *result=__real_realloc(p,n);
    if(result||!n){remove_record(p);record(result,n);}return result;
}
int __wrap_posix_memalign(void **p,size_t alignment,size_t n){
    if(n>LIMIT-used)return 12;
    int rc=__real_posix_memalign(p,alignment,n);if(!rc)record(*p,n);return rc;
}
void *__wrap_aligned_alloc(size_t alignment,size_t n){return n>LIMIT-used?NULL:record(__real_aligned_alloc(alignment,n),n);}
__attribute__((destructor)) static void report(void){fprintf(stderr,"SWITCHMUSIC_HEAP_PEAK=%zu\n",peak);}
