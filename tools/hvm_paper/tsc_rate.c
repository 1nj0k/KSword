/* Calibrate an invariant TSC against Windows QPC in the measured guest. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
int main(void) {
    LARGE_INTEGER f,a,b;int i,cpu[4];unsigned long long ta,tb;
    if(!SetThreadAffinityMask(GetCurrentThread(),1))return 1;
    QueryPerformanceFrequency(&f);__cpuid(cpu,0x80000007);
    printf("{\"kind\":\"tsc-calibration-header\",\"invariantTsc\":%s,\"qpcFrequency\":%lld}\n",(cpu[3]&(1<<8))?"true":"false",f.QuadPart);
    for(i=0;i<5;i++) {
        QueryPerformanceCounter(&a);_mm_lfence();ta=__rdtsc();_mm_lfence();
        Sleep(100);
        _mm_lfence();tb=__rdtsc();_mm_lfence();QueryPerformanceCounter(&b);
        printf("{\"kind\":\"tsc-calibration\",\"qpcBegin\":%lld,\"qpcEnd\":%lld,\"tscBegin\":%llu,\"tscEnd\":%llu,\"tscHz\":%.3f}\n",
               a.QuadPart,b.QuadPart,ta,tb,(double)(tb-ta)*(double)f.QuadPart/(double)(b.QuadPart-a.QuadPart));
    }
    return 0;
}
