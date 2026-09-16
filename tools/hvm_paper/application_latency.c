/* A paced TCP request stream records application-visible latency and completion
 * gaps around one control command. These include scheduling and socket costs;
 * they are not an exact hypervisor pause or an open-loop workload model. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <mmsystem.h>
#include <process.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static SOCKET listener;
static struct sockaddr_in address;
static volatile LONG stop_requests, worker_error;
static HANDLE ready;
static long long tick(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
static int transfer(SOCKET fd, void *data, int bytes, int writing) {
    int done=0;
    while(done<bytes) {
        int n=writing?send(fd,(const char*)data+done,bytes-done,0):recv(fd,(char*)data+done,bytes-done,0);
        if(n<=0)return 0;
        done+=n;
    }
    return 1;
}
static void configure(SOCKET fd) {
    int one=1;DWORD timeout=5000;
    setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,(const char*)&one,sizeof(one));
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,(const char*)&timeout,sizeof(timeout));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,(const char*)&timeout,sizeof(timeout));
}
static unsigned __stdcall serve(void *unused) {
    SOCKET fd;uint64_t seq,expected=1; (void)unused;
    if(!SetThreadAffinityMask(GetCurrentThread(),2)){InterlockedExchange(&worker_error,1);return 1;}
    fd=accept(listener,NULL,NULL);
    if(fd==INVALID_SOCKET){InterlockedExchange(&worker_error,2);return 1;}
    configure(fd);
    while(transfer(fd,&seq,sizeof(seq),0)) {
        if(seq!=expected++ || !transfer(fd,&seq,sizeof(seq),1)){InterlockedExchange(&worker_error,3);break;}
    }
    closesocket(fd);return 0;
}
static unsigned __stdcall request(void *unused) {
    SOCKET fd;uint64_t seq=0,reply;long long previous=0; (void)unused;
    if(!SetThreadAffinityMask(GetCurrentThread(),4)){InterlockedExchange(&worker_error,4);SetEvent(ready);return 1;}
    fd=socket(AF_INET,SOCK_STREAM,0);configure(fd);
    if(connect(fd,(struct sockaddr*)&address,sizeof(address))){InterlockedExchange(&worker_error,5);SetEvent(ready);closesocket(fd);return 1;}
    SetEvent(ready);
    while(!InterlockedCompareExchange(&stop_requests,0,0)) {
        long long begin=tick(),end;int valid;
        ++seq;reply=0;
        valid=transfer(fd,&seq,sizeof(seq),1)&&transfer(fd,&reply,sizeof(reply),0)&&reply==seq;
        end=tick();
        printf("{\"kind\":\"request\",\"seq\":%llu,\"beginQpc\":%lld,\"endQpc\":%lld,\"previousEndQpc\":%lld,\"valid\":%s}\n",
            (unsigned long long)seq,begin,end,previous,valid?"true":"false");
        if(!valid){InterlockedExchange(&worker_error,6);break;}
        previous=end;Sleep(1);
    }
    closesocket(fd);return 0;
}
int main(int argc,char **argv) {
    WSADATA wsa;HANDLE server,client,log;SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE};
    STARTUPINFOA si;PROCESS_INFORMATION pi;LARGE_INTEGER frequency;DWORD code=1,wait;
    char command[1024];int length=sizeof(address),rc=0;long long begin,end;MMRESULT timer;
    if(argc!=3 || (strcmp(argv[1],"resident-nested-hidehv") && strcmp(argv[1],"stop") && strcmp(argv[1],"status"))) {
        fprintf(stderr,"usage: application_latency <resident-nested-hidehv|stop|status> <new-control-output-path>\n");return 2;
    }
    if(!SetThreadAffinityMask(GetCurrentThread(),1) || WSAStartup(MAKEWORD(2,2),&wsa))return 3;
    QueryPerformanceFrequency(&frequency);timer=timeBeginPeriod(1);
    printf("{\"kind\":\"header\",\"schemaVersion\":1,\"command\":\"%s\",\"pid\":%lu,\"qpcFrequency\":%lld,\"timerResult\":%u,\"clientCpu\":2,\"serverCpu\":1,\"controlCpu\":0,\"pacingSleepMs\":1}\n",
        argv[1],(unsigned long)GetCurrentProcessId(),frequency.QuadPart,(unsigned)timer);
    fflush(stdout);
    listener=socket(AF_INET,SOCK_STREAM,0);memset(&address,0,sizeof(address));
    address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(bind(listener,(struct sockaddr*)&address,sizeof(address)) || listen(listener,1) || getsockname(listener,(struct sockaddr*)&address,&length))return 4;
    ready=CreateEvent(NULL,TRUE,FALSE,NULL);
    server=(HANDLE)_beginthreadex(NULL,0,serve,NULL,0,NULL);
    client=(HANDLE)_beginthreadex(NULL,0,request,NULL,0,NULL);
    if(!ready || !server || !client || WaitForSingleObject(ready,5000)!=WAIT_OBJECT_0 || worker_error)return 5;
    Sleep(3000);
    log=CreateFileA(argv[2],GENERIC_WRITE,FILE_SHARE_READ,&sa,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if(log==INVALID_HANDLE_VALUE){rc=6;goto cleanup;}
    memset(&si,0,sizeof(si));memset(&pi,0,sizeof(pi));si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;
    si.hStdOutput=log;si.hStdError=log;si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    snprintf(command,sizeof(command),"C:\\ksword\\hvm_ctl.exe --json %s",argv[1]);
    begin=tick();
    if(!CreateProcessA("C:\\ksword\\hvm_ctl.exe",command,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi)) {
        printf("{\"kind\":\"control-create-error\",\"error\":%lu}\n",(unsigned long)GetLastError());CloseHandle(log);rc=7;goto cleanup;
    }
    wait=WaitForSingleObject(pi.hProcess,30000);end=tick();
    if(wait==WAIT_OBJECT_0)GetExitCodeProcess(pi.hProcess,&code);
    /* Never terminate a possibly in-flight driver control request on timeout. */
    printf("{\"kind\":\"control\",\"beginQpc\":%lld,\"endQpc\":%lld,\"pid\":%lu,\"waitResult\":%lu,\"exitCode\":%lu}\n",
        begin,end,(unsigned long)pi.dwProcessId,(unsigned long)wait,(unsigned long)code);
    fflush(stdout);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);CloseHandle(log);
    if(wait!=WAIT_OBJECT_0 || code!=0)rc=8;
    Sleep(3000);
cleanup:
    InterlockedExchange(&stop_requests,1);
    if(WaitForSingleObject(client,6000)!=WAIT_OBJECT_0)rc=9;
    if(WaitForSingleObject(server,6000)!=WAIT_OBJECT_0)rc=10;
    printf("{\"kind\":\"final\",\"error\":%d,\"workerError\":%ld,\"endQpc\":%lld}\n",rc,worker_error,tick());
    CloseHandle(client);CloseHandle(server);CloseHandle(ready);closesocket(listener);
    if(timer==TIMERR_NOERROR)timeEndPeriod(1);
    WSACleanup();return rc?rc:(worker_error?11:0);
}
