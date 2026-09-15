/* A small, identical-source Windows/Linux workload. Results describe these
 * workloads and buffer sizes; they are not an application-performance score. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <process.h>
#include <intrin.h>
typedef SOCKET socket_t;
#define close_socket closesocket
#else
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <cpuid.h>
typedef int socket_t;
#define INVALID_SOCKET (-1)
#define close_socket close
#endif

static volatile uint64_t sink;
static double now_seconds(void) {
#ifdef _WIN32
    LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
    return (double)t.QuadPart/(double)f.QuadPart;
#else
    struct timespec t; clock_gettime(CLOCK_MONOTONIC_RAW,&t);
    return (double)t.tv_sec+(double)t.tv_nsec/1e9;
#endif
}
static uint64_t random_next(uint64_t *x) { *x^=*x<<13;*x^=*x>>7;*x^=*x<<17;return *x; }
static void cpuid_leaf(unsigned int leaf,unsigned int out[4]) {
#ifdef _WIN32
    __cpuidex((int*)out,(int)leaf,0);
#else
    __cpuid_count(leaf,0,out[0],out[1],out[2],out[3]);
#endif
}
static int pin_cpu_zero(void) {
#ifdef _WIN32
    return SetProcessAffinityMask(GetCurrentProcess(),1)?0:(int)GetLastError();
#else
    cpu_set_t s;CPU_ZERO(&s);CPU_SET(0,&s);return sched_setaffinity(0,sizeof(s),&s);
#endif
}
static void emit(const char *name,double elapsed,uint64_t operations,uint64_t bytes) {
    printf("{\"schemaVersion\":1,\"workload\":\"%s\",\"status\":\"ok\",\"seconds\":%.9f,\"operations\":%llu,\"bytes\":%llu,\"nsPerOperation\":%.4f,\"MiBPerSecond\":%.4f,\"sink\":\"%llu\"}\n",
      name,elapsed,(unsigned long long)operations,(unsigned long long)bytes,operations?elapsed*1e9/operations:0.0,elapsed>0?(double)bytes/1048576.0/elapsed:0.0,(unsigned long long)sink);
}
static int fail(const char *name,const char *reason,int error) {
    printf("{\"schemaVersion\":1,\"workload\":\"%s\",\"status\":\"error\",\"reason\":\"%s\",\"error\":%d}\n",name,reason,error);return 1;
}
static int cpu_bench(void) {
    uint64_t x=0x123456789ABCDEFULL,i,n=100000000;
    double start=now_seconds();
    for(i=0;i<n;i++) random_next(&x);
    sink=x;emit("integer-xorshift",now_seconds()-start,n,0);return 0;
}
static int exit_bench(void) {
    unsigned int out[4];uint64_t i,n=100000;double start=now_seconds();
    for(i=0;i<n;i++) {cpuid_leaf(0,out);sink+=out[0];}
    emit("cpuid-leaf0",now_seconds()-start,n,0);return 0;
}
static int memory_bench(void) {
    const size_t bytes=32U*1024U*1024U;size_t i;unsigned int r;
    unsigned char *a=(unsigned char*)malloc(bytes),*b=(unsigned char*)malloc(bytes);
    double start;
    if(!a||!b) {free(a);free(b);return fail("memcpy-32MiB","allocate",errno);}
    for(i=0;i<bytes;i++) a[i]=(unsigned char)i;
    memset(b,0,bytes);memcpy(b,a,bytes);
    start=now_seconds();
    for(r=0;r<64;r++) {memcpy(b,a,bytes);sink+=b[(r*4096U)%bytes];a[r]=(unsigned char)r;}
    emit("memcpy-32MiB",now_seconds()-start,64,(uint64_t)bytes*64); /* counts copied bytes once */
    if(memcmp(a,b,bytes)!=0) {free(a);free(b);return fail("memcpy-32MiB","verify",0);}
    free(a);free(b);return 0;
}
static int latency_bench(void) {
    const size_t count=(64U*1024U*1024U)/sizeof(uintptr_t);
    uintptr_t *nodes=(uintptr_t*)malloc(count*sizeof(*nodes));
    uint32_t *order=(uint32_t*)malloc(count*sizeof(*order));
    uintptr_t cursor;uint64_t seed=0xED5633114ULL;size_t i;double start;
    if(!nodes||!order){free(nodes);free(order);return fail("pointer-chase-64MiB","allocate",errno);}
    for(i=0;i<count;i++) order[i]=(uint32_t)i;
    for(i=count-1;i>0;i--){size_t j=(size_t)(random_next(&seed)%(i+1));uint32_t tmp=order[i];order[i]=order[j];order[j]=tmp;}
    for(i=0;i<count;i++) nodes[order[i]]=(uintptr_t)&nodes[order[(i+1)%count]];
    cursor=(uintptr_t)&nodes[order[0]];free(order);
    for(i=0;i<count;i++) cursor=*(volatile uintptr_t*)cursor;
    start=now_seconds();
    for(i=0;i<count;i++) cursor=*(volatile uintptr_t*)cursor;
    sink=(uint64_t)cursor;emit("pointer-chase-64MiB",now_seconds()-start,count,0);
    free(nodes);return 0;
}

typedef struct {socket_t listener;uint64_t bytes;int ping;int error;} net_context;
static void net_worker(net_context *c) {
    char buffer[65536];uint64_t total=0;socket_t fd=accept(c->listener,NULL,NULL);
    if(fd==INVALID_SOCKET){c->error=1;return;}
    while(total<c->bytes) {
        int want=c->ping?1:(int)((c->bytes-total)<sizeof(buffer)?c->bytes-total:sizeof(buffer));
        int got=(int)recv(fd,buffer,want,0);
        if(got<=0){c->error=2;break;}
        total+=(uint64_t)got;
        if(c->ping && send(fd,buffer,got,0)!=got){c->error=3;break;}
    }
    close_socket(fd);
}
#ifdef _WIN32
static unsigned __stdcall net_thread(void *arg){net_worker((net_context*)arg);return 0;}
#else
static void *net_thread(void *arg){net_worker((net_context*)arg);return NULL;}
#endif
static int net_bench(int ping) {
    net_context c;struct sockaddr_in address;socket_t client;char buffer[65536];
    uint64_t total=0;double start,elapsed;int addressLength=(int)sizeof(address),one=1;
#ifdef _WIN32
    HANDLE thread;WSADATA data;if(WSAStartup(MAKEWORD(2,2),&data))return fail("tcp-loopback","startup",1);
#else
    pthread_t thread;
#endif
    memset(&c,0,sizeof(c));memset(&address,0,sizeof(address));memset(buffer,0x5a,sizeof(buffer));
    c.bytes=ping?2000:64ULL*1024*1024;c.ping=ping;
    c.listener=socket(AF_INET,SOCK_STREAM,0);
    address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(bind(c.listener,(struct sockaddr*)&address,sizeof(address)) || listen(c.listener,1))return fail("tcp-loopback","listen",errno);
#ifdef _WIN32
    getsockname(c.listener,(struct sockaddr*)&address,&addressLength);
    thread=(HANDLE)_beginthreadex(NULL,0,net_thread,&c,0,NULL);
    if(!thread)return fail("tcp-loopback","thread",errno);
#else
    getsockname(c.listener,(struct sockaddr*)&address,(socklen_t*)&addressLength);
    if(pthread_create(&thread,NULL,net_thread,&c))return fail("tcp-loopback","thread",errno);
#endif
    client=socket(AF_INET,SOCK_STREAM,0);
    setsockopt(client,IPPROTO_TCP,TCP_NODELAY,(const char*)&one,sizeof(one));
    if(connect(client,(struct sockaddr*)&address,sizeof(address)))return fail("tcp-loopback","connect",errno);
    start=now_seconds();
    while(total<c.bytes) {
        int want=ping?1:(int)((c.bytes-total)<sizeof(buffer)?c.bytes-total:sizeof(buffer));
        int sent=(int)send(client,buffer,want,0);
        if(sent<=0)return fail("tcp-loopback","send",errno);
        total+=(uint64_t)sent;
        if(ping && recv(client,buffer,1,0)!=1)return fail("tcp-loopback","recv",errno);
    }
    close_socket(client);
#ifdef _WIN32
    WaitForSingleObject(thread,INFINITE);CloseHandle(thread);
#else
    pthread_join(thread,NULL);
#endif
    elapsed=now_seconds()-start;close_socket(c.listener);
    if(c.error)return fail("tcp-loopback","worker",c.error);
    emit(ping?"tcp-loopback-rtt-1B":"tcp-loopback-64MiB",elapsed,ping?c.bytes:0,ping?0:c.bytes);
    return 0;
}
static int disk_bench(const char *path) {
    const size_t chunk=1024U*1024U;const unsigned int rounds=64;unsigned int i;
    void *buffer;double start;int rc=0;
#ifdef _WIN32
    HANDLE file;DWORD transferred;
    buffer=VirtualAlloc(NULL,chunk,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!buffer)return fail("disk-direct","allocate",(int)GetLastError());
    file=CreateFileA(path,GENERIC_READ|GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_FLAG_NO_BUFFERING|FILE_FLAG_WRITE_THROUGH,NULL);
    if(file==INVALID_HANDLE_VALUE){VirtualFree(buffer,0,MEM_RELEASE);return fail("disk-direct","create-new",(int)GetLastError());}
#else
    int file;if(posix_memalign(&buffer,4096,chunk))return fail("disk-direct","allocate",errno);
    file=open(path,O_RDWR|O_CREAT|O_EXCL|O_DIRECT|O_DSYNC,0600);
    if(file<0){free(buffer);return fail("disk-direct","create-new",errno);}
#endif
    memset(buffer,0x3c,chunk);start=now_seconds();
    for(i=0;i<rounds;i++) {
#ifdef _WIN32
        if(!WriteFile(file,buffer,(DWORD)chunk,&transferred,NULL)||transferred!=chunk){rc=1;break;}
#else
        if(write(file,buffer,chunk)!=(ssize_t)chunk){rc=1;break;}
#endif
    }
#ifdef _WIN32
    if(!FlushFileBuffers(file))rc=1;
    SetFilePointer(file,0,NULL,FILE_BEGIN);
#else
    if(fsync(file))rc=1;
    lseek(file,0,SEEK_SET);
#endif
    if(!rc)emit("disk-direct-write-64MiB",now_seconds()-start,rounds,(uint64_t)rounds*chunk);
    start=now_seconds();
    for(i=0;!rc&&i<rounds;i++) {
#ifdef _WIN32
        if(!ReadFile(file,buffer,(DWORD)chunk,&transferred,NULL)||transferred!=chunk){rc=1;break;}
#else
        if(read(file,buffer,chunk)!=(ssize_t)chunk){rc=1;break;}
#endif
        if(((unsigned char*)buffer)[0]!=0x3c||((unsigned char*)buffer)[chunk-1]!=0x3c)rc=1;
    }
    if(!rc)emit("disk-direct-read-64MiB",now_seconds()-start,rounds,(uint64_t)rounds*chunk);
#ifdef _WIN32
    CloseHandle(file);DeleteFileA(path);VirtualFree(buffer,0,MEM_RELEASE);
#else
    close(file);unlink(path);free(buffer);
#endif
    return rc?fail("disk-direct","io-or-verification",errno):0;
}
int main(int argc,char **argv) {
    unsigned int leaf[4];int rc,pinned=pin_cpu_zero();
    setvbuf(stdout,NULL,_IONBF,0);
    if(argc<2){fprintf(stderr,"usage: microbench cpu|cpuid|memory|latency|net|ping|disk <new-file>\n");return 2;}
    cpuid_leaf(1,leaf);
    printf("{\"schemaVersion\":1,\"kind\":\"metadata\",\"pointerBytes\":%u,\"pinCpu\":0,\"affinityResult\":%d,\"cpuid1Eax\":\"0x%08X\",\"cpuid1Ecx\":\"0x%08X\",\"timer\":\"%s\"}\n",(unsigned)sizeof(void*),pinned,leaf[0],leaf[2],
#ifdef _WIN32
      "QueryPerformanceCounter"
#else
      "CLOCK_MONOTONIC_RAW"
#endif
    );
    if(!strcmp(argv[1],"cpu"))rc=cpu_bench();
    else if(!strcmp(argv[1],"cpuid"))rc=exit_bench();
    else if(!strcmp(argv[1],"memory"))rc=memory_bench();
    else if(!strcmp(argv[1],"latency"))rc=latency_bench();
    else if(!strcmp(argv[1],"net"))rc=net_bench(0);
    else if(!strcmp(argv[1],"ping"))rc=net_bench(1);
    else if(!strcmp(argv[1],"disk")&&argc==3)rc=disk_bench(argv[2]);
    else rc=2;
    return rc;
}
