/* Host-only parent directory gate tests; no modem connection or mutation. */
#include "../native/item_probe.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum scenario { SUCCESS, MISSING, DENIED, REGULAR_FILE, ITEM_FILE, SYMLINK,
    SHORT_REPLY, LONG_REPLY, WRONG_HEADER, TRANSPORT_ERROR, CHANGED_MODE };
static enum scenario scenario;
static unsigned int calls, fail_at;
static const char *const paths[] = {
    "/nv/item_files/modem/lte/rrc/csp",
    "/nv/item_files/modem/lte/rrc/efs",
    "/nv/item_files/modem/mmode"
};
static void p32(unsigned char *p, uint32_t v)
{
    p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8);
    p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24);
}
int diag_exchange(const unsigned char *req, size_t n, unsigned char *resp,
                  size_t cap, size_t *actual, char *err, size_t errcap)
{
    enum scenario current;
    assert(calls < 6);
    assert(req[0]==0x4b && req[1]==0x13 && req[2]==(calls % 2 == 0 ? 16 : 15) && req[3]==0);
    assert(n==4+strlen(paths[calls / 2])+1);
    assert(memcmp(req+4,paths[calls / 2],n-4)==0);
    current=calls++==fail_at?scenario:SUCCESS;
    assert(cap>=33);
    memset(resp,0,cap); memcpy(resp,req,4); p32(resp+8,0040700u); *actual=req[2]==16?24:32;
    switch (current) {
    case SUCCESS: break;
    case MISSING: p32(resp+4,2); break;
    case DENIED: p32(resp+4,13); break;
    case REGULAR_FILE: p32(resp+8,0100600u); break;
    case ITEM_FILE: p32(resp+8,0160600u); break;
    case SYMLINK: p32(resp+8,0120777u); break;
    case SHORT_REPLY: --*actual; break;
    case LONG_REPLY: ++*actual; break;
    case WRONG_HEADER: resp[1]=0x3e; break;
    case TRANSPORT_ERROR:
        if (err!=NULL && errcap!=0) (void)snprintf(err,errcap,"mock_timeout");
        return -1;
    case CHANGED_MODE: p32(resp+8,0040755u); break;
    }
    return 0;
}
int main(void)
{
    char error[256]; unsigned int tests=0;
    for (fail_at=0;fail_at<6;++fail_at) {
        for (scenario=SUCCESS;scenario<=CHANGED_MODE;scenario=(enum scenario)(scenario+1)) {
            calls=0; memset(error,0x5a,sizeof error);
            assert(item_parent_directories_ready(error,sizeof error)==(scenario==SUCCESS?0:-1));
            assert(calls==(scenario==SUCCESS?6:scenario==CHANGED_MODE?(fail_at / 2 + 1) * 2:fail_at+1));
            if (scenario==SUCCESS) assert(error[0]=='\0');
            else assert(strstr(error,paths[fail_at / 2])!=NULL);
            if (scenario==MISSING) assert(strstr(error,"errno=2")!=NULL);
            if (scenario==DENIED) assert(strstr(error,"errno=13")!=NULL);
            ++tests;
        }
    }
    calls=0; fail_at=0; scenario=MISSING; error[0]='x'; error[1]='y';
    assert(item_parent_directories_ready(error,1)==-1);
    assert(error[0]=='\0' && error[1]=='y' && calls==1);
    printf("item_parent: %u protocol scenarios plus output bound passed\n",tests);
    return 0;
}
