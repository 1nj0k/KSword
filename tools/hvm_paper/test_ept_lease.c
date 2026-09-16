/* Test real translation identities, including reuse, large pages and read failure. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../KswordARKDriver/src/features/hvm/hvm_nested_lease_walk.h"

typedef struct {
    KSW_LEASE_U64 tables[4][512];
    KSW_LEASE_U64 fail_address;
} fixture;
static int read_entry(void *context, KSW_LEASE_U64 address, KSW_LEASE_U64 *value) {
    fixture *f=(fixture*)context;
    unsigned int table=(unsigned int)(address/4096)-1;
    if(address==f->fail_address || address<4096 || table>=4 || (address&7)) return 0;
    *value=f->tables[table][(address&4095)/8];return 1;
}
static void reset(fixture *f) {
    memset(f,0,sizeof(*f));
    f->tables[0][0]=0x2007;f->tables[1][0]=0x3007;
    f->tables[2][0]=0x4007;f->tables[3][7]=0xABC037;
}
int main(void) {
    fixture f;
    KSW_HVM_PAGE_TRANSLATION t;
    unsigned int level,bit;
    reset(&f);
    assert(KswordHvmLeaseCapture(0x105E,0x7000,read_entry,&f,&t));
    assert(t.SourcePage==0xABC000 && t.EntryCount==4 && t.Permissions==7);
    assert(t.EntryAddress[0]==0x1000 && t.EntryAddress[3]==0x4038);
    assert(KswordHvmLeaseValidate(&t,read_entry,&f)==1);
    /* Exhaustively distinguish semantic bits from the only allowed metadata bits. */
    for(level=0;level<4;level++) for(bit=0;bit<64;bit++) {
        KSW_LEASE_U64 *entry=&f.tables[level][level==3?7:0];
        int ignored=bit==8 || (level==3 && bit==9);
        *entry^=1ULL<<bit;
        assert(KswordHvmLeaseValidate(&t,read_entry,&f)==ignored);
        *entry^=1ULL<<bit;
    }
    f.fail_address=t.EntryAddress[2];
    assert(KswordHvmLeaseValidate(&t,read_entry,&f)==-1);
    reset(&f);
    f.tables[3][7]=0xFED037; /* same root/GPA, different backing */
    assert(KswordHvmLeaseValidate(&t,read_entry,&f)==0);
    reset(&f);
    f.tables[1][0]=0x4007; /* same root, changed interior path */
    assert(KswordHvmLeaseValidate(&t,read_entry,&f)==0);
    reset(&f);
    f.tables[2][0]=0xA000B7; /* 2-MiB leaf */
    assert(KswordHvmLeaseCapture(0x105E,0x7000,read_entry,&f,&t));
    assert(t.EntryCount==3 && t.SourcePage==0xA07000);
    f.tables[2][0]|=0x300;
    assert(KswordHvmLeaseValidate(&t,read_entry,&f)==1);
    f.tables[2][0]^=2;
    assert(KswordHvmLeaseValidate(&t,read_entry,&f)==0);
    reset(&f);
    f.tables[1][0]=0x800000B7; /* 1-GiB leaf */
    assert(KswordHvmLeaseCapture(0x105E,0x07000000,read_entry,&f,&t));
    assert(t.EntryCount==2 && t.SourcePage==0x87000000);
    reset(&f);
    assert(KswordHvmLeaseCapture(0x101E,0x7000,read_entry,&f,&t));
    f.tables[3][7]|=0x100; /* A/D disabled: do not ignore these bits */
    assert(KswordHvmLeaseValidate(&t,read_entry,&f)==0);
    reset(&f);f.tables[3][7]=0xABC007; /* non-WB memory */
    assert(!KswordHvmLeaseCapture(0x105E,0x7000,read_entry,&f,&t));
    reset(&f);f.tables[2][0]=0xA010B7; /* misaligned large page */
    assert(!KswordHvmLeaseCapture(0x105E,0x7000,read_entry,&f,&t));
    reset(&f);f.tables[0][0]=0x2087; /* illegal PML4 large bit */
    assert(!KswordHvmLeaseCapture(0x105E,0x7000,read_entry,&f,&t));
    reset(&f);f.tables[3][7]=0xABC032; /* write without read */
    assert(!KswordHvmLeaseCapture(0x105E,0x7000,read_entry,&f,&t));
    reset(&f);
    assert(!KswordHvmLeaseCapture(0x105E,0x7001,read_entry,&f,&t));
    assert(!KswordHvmLeaseCapture(0x105E,1ULL<<48,read_entry,&f,&t));
    assert(!KswordHvmLeaseCapture(0x105F,0x7000,read_entry,&f,&t));
    puts("EPT_LEASE=PASS: identity drift, all bits, A/D, large pages, unreadable sources, invalid requests");
    return 0;
}
