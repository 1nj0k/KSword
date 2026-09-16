/* User-visible scheduling gaps around an existing HVM CLI operation.
 * This is NOT an internal VMX stage timer or an exact pause measurement.
 * QPC samples include Windows scheduling, Hyper-V scheduling and observer cost.
 * No driver IOCTLs, VMware APIs, process injection or hooks are used here. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_CPUS 32
#define TOP_GAPS 16
typedef struct { LONGLONG start,end; } gap;
typedef struct {
    DWORD cpu,affinity_error;
    uint64_t samples;
    gap top[TOP_GAPS];
    LONGLONG first,last;
} cpu_result;
static volatile LONG running=1,ready=0;
static HANDLE go_event;
static cpu_result results[MAX_CPUS];
static DWORD WINAPI observe(void *arg) {
    cpu_result *r=(cpu_result*)arg;
    LARGE_INTEGER previous,current;
    int i,min_i;
    if(!SetThreadAffinityMask(GetCurrentThread(),(DWORD_PTR)1<<r->cpu))
        r->affinity_error=GetLastError();
    InterlockedIncrement(&ready);
    WaitForSingleObject(go_event,INFINITE);
    QueryPerformanceCounter(&previous);r->first=previous.QuadPart;
    while(InterlockedCompareExchange(&running,1,1)) {
        QueryPerformanceCounter(&current);r->samples++;
        if(current.QuadPart-previous.QuadPart>1000) {
            min_i=0;
            for(i=1;i<TOP_GAPS;i++)
                if(r->top[i].end-r->top[i].start<r->top[min_i].end-r->top[min_i].start) min_i=i;
            if(current.QuadPart-previous.QuadPart>r->top[min_i].end-r->top[min_i].start) {
                r->top[min_i].start=previous.QuadPart;r->top[min_i].end=current.QuadPart;
            }
        }
        previous=current;
    }
    r->last=previous.QuadPart;return 0;
}
int main(int argc,char **argv) {
    DWORD count=GetActiveProcessorCount(ALL_PROCESSOR_GROUPS),i,code=1,launch_error=0;
    HANDLE threads[MAX_CPUS]={0};
    STARTUPINFOA si={0};PROCESS_INFORMATION pi={0};
    LARGE_INTEGER frequency,command_start,command_end;
    char command[256];
    if(argc!=2 || (strcmp(argv[1],"resident-nested-hidehv") && strcmp(argv[1],"stop"))) return 2;
    if(count>MAX_CPUS || GetActiveProcessorGroupCount()!=1) return 3;
    QueryPerformanceFrequency(&frequency);
    go_event=CreateEventW(NULL,TRUE,FALSE,NULL);if(!go_event)return 4;
    for(i=0;i<count;i++) {
        results[i].cpu=i;threads[i]=CreateThread(NULL,0,observe,&results[i],0,NULL);
        if(!threads[i]) {InterlockedExchange(&running,0);SetEvent(go_event);return 5;}
    }
    while((DWORD)InterlockedCompareExchange(&ready,0,0)<count) Sleep(1);
    SetEvent(go_event);Sleep(1000);
    snprintf(command,sizeof(command),"C:\\ksword\\hvm_ctl.exe --json %s",argv[1]);
    si.cb=sizeof(si);
    QueryPerformanceCounter(&command_start);
    if(CreateProcessA(NULL,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi)) {
        if(WaitForSingleObject(pi.hProcess,30000)==WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess,&code);
        else code=258; /* Never kill an in-flight HVM transition. */
        CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
    }else launch_error=GetLastError();
    QueryPerformanceCounter(&command_end);Sleep(1000);
    InterlockedExchange(&running,0);WaitForMultipleObjects(count,threads,TRUE,INFINITE);
    printf("{\"schemaVersion\":1,\"kind\":\"execution-gap-observer\",\"command\":\"%s\",\"commandExitCode\":%lu,\"launchError\":%lu,\"qpcFrequency\":%lld,\"commandStartQpc\":%lld,\"commandEndQpc\":%lld,\"processors\":[",argv[1],(unsigned long)code,(unsigned long)launch_error,(long long)frequency.QuadPart,(long long)command_start.QuadPart,(long long)command_end.QuadPart);
    for(i=0;i<count;i++) {
        int j;cpu_result *r=&results[i];
        printf("%s{\"group\":0,\"number\":%lu,\"affinityError\":%lu,\"samples\":%llu,\"firstQpc\":%lld,\"lastQpc\":%lld,\"largestGaps\":[",i?",":"",(unsigned long)i,(unsigned long)r->affinity_error,(unsigned long long)r->samples,(long long)r->first,(long long)r->last);
        for(j=0;j<TOP_GAPS;j++) printf("%s{\"startQpc\":%lld,\"endQpc\":%lld}",j?",":"",(long long)r->top[j].start,(long long)r->top[j].end);
        printf("]}");CloseHandle(threads[i]);
    }
    printf("]}\n");CloseHandle(go_event);return code?1:0;
}
