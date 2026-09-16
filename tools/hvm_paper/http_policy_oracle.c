/* Validate the complete fixed-schema policy document returned by real httpd.
 * This fixture oracle rejects corruption even when HTTP status is still 200. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <time.h>

int main(int argc,char **argv) {
    static const char prefix[]="{\"product\":\"research-fixture\",\"price\":1250,\"padding\":\"";
    unsigned char data[4097];size_t n,i;FILE *f;int valid;struct timespec now;
    if(argc!=2)return 2;
    f=fopen(argv[1],"rb");if(!f)return 3;
    n=fread(data,1,sizeof(data),f);if(ferror(f)){fclose(f);return 4;}fclose(f);
    valid=n==4096 && !memcmp(data,prefix,sizeof(prefix)-1) && !memcmp(data+4093,"\"}\n",3);
    if(valid)for(i=sizeof(prefix)-1;i<4093;++i)if(data[i]!=' '){valid=0;break;}
    clock_gettime(CLOCK_MONOTONIC,&now);
    printf("{\"kind\":\"policy-oracle\",\"uptime\":%lld.%09ld,\"bytes\":%zu,\"validPolicy\":%s,\"decision\":\"%s\"}\n",
        (long long)now.tv_sec,now.tv_nsec,n,valid?"true":"false",valid?"quote-1250":"reject-corrupt-policy");
    return 0; /* A recorded rejection is a valid fault-injection observation. */
}
