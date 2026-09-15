#pragma once
#include "KswordArkHvmIoctl.h"

/* One wire-request builder for native GUI controls and the command engine.
 * The driver remains authoritative for allowed flags and lifecycle state. */
static __inline void KswordArkHvmBuildControlRequest(
    KSWORD_ARK_CONTROL_HVM_REQUEST* request,
    unsigned long command, unsigned long flags,
    unsigned long generation, unsigned long parameter)
{
    unsigned long i;
    unsigned char* bytes = (unsigned char*)request;
    for (i = 0; i < (unsigned long)sizeof(*request); ++i) { bytes[i] = 0; }
    request->version = KSWORD_ARK_HVM_PROTOCOL_VERSION;
    request->size = (unsigned long)sizeof(*request);
    request->command = command;
    request->flags = flags;
    if (command == KSWORD_ARK_HVM_CONTROL_LAUNCH_TEST_GUEST)
    {
        request->flags |= KSWORD_ARK_HVM_CONTROL_FLAG_ONE_SHOT_GUEST;
    }
    request->confirmationToken = KSWORD_ARK_HVM_CONTROL_CONFIRMATION_TOKEN;
    request->expectedGeneration = generation;
    if (command == KSWORD_ARK_HVM_CONTROL_SOAK) { request->soakMilliseconds = parameter; }
    if ((flags & KSWORD_ARK_HVM_CONTROL_FLAG_VMREAD_BENCH) != 0)
    {
        request->vmreadBenchIterations = parameter;
    }
}
