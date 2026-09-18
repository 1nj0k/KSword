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
/* Flip every bit of every captured entry and require the ignored set to be exact.
   Accessed is metadata at any level; dirty is defined only at the terminating
   leaf, so the leaf level tolerates one more bit than its parents.  A walk that
   stops early has fewer levels to sweep, which is why the leaf level is passed
   in rather than assumed to be the last table. */
static void sweep(fixture *f,const KSW_HVM_PAGE_TRANSLATION *t,
                  const unsigned int *slots,unsigned int leaf_level) {
    unsigned int level,bit;
    for(level=0;level<t->EntryCount;level++) for(bit=0;bit<64;bit++) {
        KSW_LEASE_U64 *entry=&f->tables[level][slots[level]];
        int ignored=bit==8 || (level==leaf_level && bit==9);
        *entry^=1ULL<<bit;
        assert(KswordHvmLeaseValidate(t,read_entry,f)==ignored);
        *entry^=1ULL<<bit;
    }
}
/* The 2-MiB and 1-GiB leaves terminate the walk early, so every property the
   ordinary path proves has to be proved again against a shorter path: a short
   EntryCount must not leave later levels unchecked, and a leaf two or three
   levels up must still expire on permission, cache-type or address drift.
   Before this the large-page cases were capture-only, which proves the walk
   reaches a leaf and nothing about whether the lease ever expires. */
static void large_pages(void) {
    fixture f;KSW_HVM_PAGE_TRANSLATION t,u;
    static const unsigned int three[3]={0,0,0},two[2]={0,0};

    /* A 2-MiB leaf: drift detection across the whole shortened path. */
    reset(&f);f.tables[2][0]=0xA000B7;
    assert(KswordHvmLeaseCapture(0x105E,0x7000,read_entry,&f,&t));
    assert(t.EntryCount==3 && t.SourcePage==0xA07000 && t.Permissions==7);
    assert(t.EntryAddress[2]==0x3000 && t.EntryAddress[3]==0);
    sweep(&f,&t,three,2);
    /* A read that fails at the leaf is unverifiable, not proven unchanged. */
    f.fail_address=t.EntryAddress[2];
    assert(KswordHvmLeaseValidate(&t,read_entry,&f)==-1);
    f.fail_address=0;
    /* The unread levels below the leaf must not be consulted. */
    f.tables[3][7]=0xDEAD037;
    assert(KswordHvmLeaseValidate(&t,read_entry,&f)==1);

    /* Two pages inside one 2-MiB leaf share a path but not a source frame. */
    reset(&f);f.tables[2][0]=0xA000B7;
    assert(KswordHvmLeaseCapture(0x105E,0x9000,read_entry,&f,&u));
    assert(u.SourcePage==0xA09000 && u.SourcePage-t.SourcePage==0x2000);
    assert(u.EntryCount==t.EntryCount &&
           !memcmp(u.EntryAddress,t.EntryAddress,sizeof(u.EntryAddress)));
    /* Both still validate: the lease proves path identity, never GPA identity.
       That is the documented limit, and it is asserted here so that a future
       change which silently made validation GPA-sensitive would be noticed. */
    assert(KswordHvmLeaseValidate(&t,read_entry,&f)==1);
    assert(KswordHvmLeaseValidate(&u,read_entry,&f)==1);

    /* A 1-GiB leaf: the shortest accepted path, previously never validated. */
    reset(&f);f.tables[1][0]=0x800000B7;
    assert(KswordHvmLeaseCapture(0x105E,0x07000000,read_entry,&f,&t));
    assert(t.EntryCount==2 && t.SourcePage==0x87000000 && t.Permissions==7);
    assert(t.EntryAddress[1]==0x2000 && t.EntryAddress[2]==0 && t.EntryAddress[3]==0);
    assert(KswordHvmLeaseValidate(&t,read_entry,&f)==1);
    sweep(&f,&t,two,1);
    f.fail_address=t.EntryAddress[1];
    assert(KswordHvmLeaseValidate(&t,read_entry,&f)==-1);
    f.fail_address=0;
    /* Two levels of stale table state must stay outside the verification. */
    f.tables[2][0]=0xBAD037;f.tables[3][7]=0xBAD037;
    assert(KswordHvmLeaseValidate(&t,read_entry,&f)==1);

    /* With A/D disabled, a large leaf's dirty bit is not hardware metadata. */
    reset(&f);f.tables[1][0]=0x800000B7;
    assert(KswordHvmLeaseCapture(0x101E,0x07000000,read_entry,&f,&t));
    f.tables[1][0]|=0x200;
    assert(KswordHvmLeaseValidate(&t,read_entry,&f)==0);
    reset(&f);f.tables[2][0]=0xA000B7;
    assert(KswordHvmLeaseCapture(0x101E,0x7000,read_entry,&f,&t));
    f.tables[2][0]|=0x100;
    assert(KswordHvmLeaseValidate(&t,read_entry,&f)==0);

    /* Rejections that only a 1-GiB leaf can express. */
    reset(&f);f.tables[1][0]=0x800010B7; /* address bit inside the 1-GiB offset */
    assert(!KswordHvmLeaseCapture(0x105E,0x07000000,read_entry,&f,&t));
    reset(&f);f.tables[1][0]=0x80000087; /* 1-GiB leaf, non-WB memory type */
    assert(!KswordHvmLeaseCapture(0x105E,0x07000000,read_entry,&f,&t));
    reset(&f);f.tables[1][0]=0x800000B6; /* 1-GiB leaf, write without read */
    assert(!KswordHvmLeaseCapture(0x105E,0x07000000,read_entry,&f,&t));
    reset(&f);f.tables[3][7]=0xABC0B7;   /* large bit is illegal at the PTE level */
    assert(!KswordHvmLeaseCapture(0x105E,0x7000,read_entry,&f,&t));

    /* A parent restriction still binds a leaf that terminates above the PTE. */
    reset(&f);f.tables[0][0]=0x2005;f.tables[2][0]=0xA000B7;
    assert(KswordHvmLeaseCapture(0x105E,0x7000,read_entry,&f,&t));
    assert(t.Permissions==5);
    /* An empty intersection is refused even though the leaf itself is readable:
       an execute-only parent over a read/write 1-GiB leaf grants nothing. */
    reset(&f);f.tables[0][0]=0x2004;f.tables[1][0]=0x800000B3;
    assert(!KswordHvmLeaseCapture(0x105E,0x07000000,read_entry,&f,&t));
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
    large_pages();
    puts("EPT_LEASE=PASS: identity drift, all bits, A/D, unreadable sources, invalid requests, "
         "2-MiB and 1-GiB leaves (drift, bit sweep, offset folding, short-path bounds)");
    return 0;
}
