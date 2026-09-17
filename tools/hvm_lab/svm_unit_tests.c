#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include "../../KswordARKDriver/src/features/hvm/hvm_svm_arch.h"

static unsigned checks;
static int check(int condition, unsigned line, const char* expression)
{
    ++checks;
    if (!condition) { printf("FAIL line %u: %s\n", line, expression); }
    return condition;
}
#define CHECK(x) do { if (!check((x), (unsigned)__LINE__, #x)) { return 1; } } while (0)
int main(void)
{
    KSW_SVM_VMCB v;
    unsigned bank, msr, pat, level, bits, seen, active, start;
    const unsigned bases[] = {0, 0xc0000000U, 0xc0010000U};
    CHECK(sizeof(v) == 4096);
    CHECK(sizeof(KSW_SVM_SEGMENT) == 16);
    memset(&v, 0xa5, sizeof(v));
    KswSvmWrite64(&v, KSW_VMCB_EXITCODE, ~0ULL);
    CHECK(KswSvmRead64(&v, KSW_VMCB_EXITCODE) == ~0ULL);
    CHECK(KswSvmRead64(&v, KSW_VMCB_EXITINFO1) == 0xa5a5a5a5a5a5a5a5ULL);
    KswSvmWrite32(&v, KSW_VMCB_ASID, 1);
    CHECK(((unsigned char*)&v)[KSW_VMCB_TLB] == 0xa5);
    CHECK(KSW_VMCB_RAX == 0x5f8 && KSW_VMCB_RSP == 0x5d8 && KSW_VMCB_RIP == 0x578);
    for (bank = 0; bank < 3; ++bank) {
        for (msr = 0; msr < 8192; ++msr) {
            unsigned bit = KswSvmMsrpmBit(bases[bank] + msr, 0);
            CHECK(bit / 8 >= bank * 2048 && bit / 8 < (bank + 1) * 2048);
            CHECK(KswSvmMsrpmBit(bases[bank] + msr, 1) == bit + 1);
            if (msr) { CHECK(bit == KswSvmMsrpmBit(bases[bank] + msr - 1, 1) + 1); }
        }
        CHECK(KswSvmMsrpmBit(bases[bank] + 8192, 0) == 0xffffffffU);
    }
    for (bits = 0; bits < 65; ++bits) {
        KSW_SVM_U64 mask = KswNptAddressMask(bits);
        CHECK((mask != 0) == (bits >= 32 && bits <= 48));
        if (mask) { CHECK((mask & 4095) == 0 && ((mask | 4095) + 1) == (1ULL << bits)); }
    }
    for (level = 1; level <= 3; ++level) {
        for (pat = 0; pat < 8; ++pat) {
            KSW_SVM_U64 f = KswNptLeafFlags(level, pat);
            unsigned decoded = (unsigned)((f >> 3) & 3) | (unsigned)(((f >> (level == 1 ? 7 : 12)) & 1) << 2);
            CHECK(decoded == pat && (f & 7) == 7);
            CHECK((f & (1ULL << 63)) == 0);
            if (level != 1) { CHECK((f & 128) != 0); }
        }
    }
    CHECK(!KswNptLeafFlags(0, 0) && !KswNptLeafFlags(4, 0) && !KswNptLeafFlags(1, 8));
    CHECK(!KswSvmNextRipValid(~0ULL, 0) && !KswSvmNextRipValid(7, 7));
    CHECK(KswSvmNextRipValid(7, 22) && !KswSvmNextRipValid(7, 23));
    for (seen = 0; seen < 4; ++seen) for (active = 0; active < 2; ++active) for (start = 0; start < 2; ++start) {
        CHECK(KswSvmParticipantValid(seen, active, start) == (unsigned)(seen == 1 && active == start));
    }
    /* A duplicate cannot compensate for a missing CPU even with equal totals. */
    CHECK(!KswSvmParticipantValid(2, 1, 1) && !KswSvmParticipantValid(0, 1, 1));
    CHECK(!KswSvmAsidValid(0,1) && !KswSvmAsidValid(1,1) && KswSvmAsidValid(2,1));
    CHECK(!KswSvmAsidValid(65536,0) && KswSvmAsidValid(65536,65535) && !KswSvmAsidValid(65536,65536));
    CHECK(KswSvmExceptionEvent(6)==0x80000306ULL && KswSvmExceptionEvent(13)==0x80000b0dULL);
    CHECK(!KswSvmExceptionEvent(14));
    {
        int r[4]={1,2,-1,4};
        KswSvmFilterCpuid(0x80000001U,r);
        CHECK(r[0]==1 && r[1]==2 && r[2]==-5 && r[3]==4);
        KswSvmFilterCpuid(0x40000000U,r);
        CHECK(r[0]==1 && r[1]==2 && r[2]==-5 && r[3]==4);
        KswSvmFilterCpuid(0x8000000aU,r);
        CHECK(!(r[0]|r[1]|r[2]|r[3]));
    }
    printf("SVM_LOGIC_CHECKS=%u RESULT=PASS (no hardware executed)\n", checks);
    return 0;
}
