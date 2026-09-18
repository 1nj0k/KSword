/* Exercise the production formatter without opening a device or executing IOCTLs. */
#include <windows.h>
#include <string.h>
#include "../../shared/driver/KswordArkHvmIoctl.h"

static BOOL WINAPI FakeDeviceIoControl(HANDLE device, DWORD code, LPVOID input,
    DWORD inputSize, LPVOID output, DWORD outputSize, LPDWORD returned,
    LPOVERLAPPED overlapped)
{
    KSWORD_ARK_QUERY_HVM_RESPONSE* response = (KSWORD_ARK_QUERY_HVM_RESPONSE*)output;
    (void)device; (void)code; (void)input; (void)inputSize; (void)overlapped;
    if (outputSize != sizeof(*response)) { return FALSE; }
    memset(response, 0, sizeof(*response));
    response->backend = 2;
    response->slatType = 2;
    response->svmCapabilities.asidCount = 64;
    *returned = sizeof(*response);
    return TRUE;
}

#include "../../Ksword5.1/Ksword5.1/ArkDriverClient/HvmCommandCatalog.c"
#define DeviceIoControl FakeDeviceIoControl
#include "../../Ksword5.1/Ksword5.1/ArkDriverClient/HvmCommandEngine.c"
#undef DeviceIoControl

int main(int argc, char** argv)
{
    if (argc == 1 || strcmp(argv[1], "strings") != 0) { return DoQuery(NULL, 1); }
    KswordHvmPrintJsonString("没有拒绝过\"\\\n\t\xf0\x9f\x98\x80");
    putchar('\n');
    KswordHvmPrintJsonString("\xff\xe8");
    putchar('\n');
    return 0;
}
