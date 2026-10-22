/* Host-only mutation protocol tests. Every exchange is mocked. */
#include "../native/item_probe.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum scenario { PUT_OK, PUT_OPAQUE_RECEIPT, PUT_GET_MISSING, PUT_GET_WRONG_DATA,
    PUT_GET_WRONG_LENGTH, PUT_GET_LONG_LENGTH, PUT_GET_ZERO_LENGTH,
    PUT_GET_BAD_SEQUENCE, PUT_GET_BAD_FRAME, PUT_GET_TRANSPORT,
    PUT_SEND_FAIL, PUT_BAD_HEADER, PUT_SHORT_RECEIPT, PUT_OVERSIZE_RECEIPT,
    UNLINK_OK, UNLINK_ALREADY_ABSENT, UNLINK_DENIED, UNLINK_GET_PRESENT,
    UNLINK_GET_DENIED, UNLINK_GET_FAIL, UNLINK_SEND_FAIL, UNLINK_BAD_HEADER,
    UNLINK_BAD_LENGTH };
static enum scenario scenario;
static unsigned int calls;
static unsigned int item_index;
static unsigned char expected_data[36];
static const char *const paths[] = {
    "/nv/item_files/modem/lte/rrc/csp/pci_lock",
    "/nv/item_files/modem/lte/rrc/efs/cell_restrict_opt_params"
};
static uint16_t put_sequence;
static uint32_t u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static void p32(unsigned char *p, uint32_t v)
{
    p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8);
    p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24);
}
int diag_exchange(const unsigned char *req, size_t n, unsigned char *resp,
                  size_t cap, size_t *actual, char *err, size_t errcap)
{
    int is_put = scenario < UNLINK_OK;
    const char *path = paths[item_index];
    size_t path_size = strlen(path) + 1, data_size = item_index == 0 ? 4u : 36u;
    assert(calls < 2);
    memset(resp, 0, cap);
    assert(req[0]==0x4b && req[1]==0x13 && req[3]==0);
    memcpy(resp, req, 4);
    if (calls++ == 0) {
        assert(cap == 64);
        if (is_put) {
            assert(req[2]==38 && n==14+data_size+path_size);
            assert(u32(req+4)==data_size && u32(req+8)==0x80040);
            assert(memcmp(req+14,expected_data,data_size)==0);
            assert(memcmp(req+14+data_size,path,path_size)==0);
            put_sequence=(uint16_t)((uint16_t)req[12]|((uint16_t)req[13]<<8));
            assert(put_sequence != 0);
            *actual=10;
            if (scenario==PUT_OPAQUE_RECEIPT) memset(resp+4,0xff,6);
            if (scenario==PUT_SEND_FAIL) {
                (void)snprintf(err,errcap,"mock_send_failure:errno=5");
                return -1;
            }
            if (scenario==PUT_BAD_HEADER) resp[1]=0x3e;
            if (scenario==PUT_SHORT_RECEIPT) *actual=3;
            if (scenario==PUT_OVERSIZE_RECEIPT) *actual=cap+1;
        } else {
            assert(req[2]==8 && n==4+path_size);
            assert(memcmp(req+4,path,path_size)==0);
            *actual=8;
            if (scenario==UNLINK_ALREADY_ABSENT) p32(resp+4,2);
            if (scenario==UNLINK_DENIED) p32(resp+4,13);
            if (scenario==UNLINK_SEND_FAIL) {
                (void)snprintf(err,errcap,"mock_send_failure:errno=5");
                return -1;
            }
            if (scenario==UNLINK_BAD_HEADER) resp[2]=9;
            if (scenario==UNLINK_BAD_LENGTH) *actual=9;
        }
    } else {
        assert(req[2]==39 && cap==270 && n==14+path_size);
        assert(u32(req+4)==data_size && u32(req+8)==path_size);
        assert(memcmp(req+14,path,path_size)==0);
        if (is_put) assert((uint16_t)((uint16_t)req[12]|((uint16_t)req[13]<<8)) != put_sequence);
        memcpy(resp+12,req+12,2);
        *actual=14+data_size; p32(resp+4,(uint32_t)data_size); memcpy(resp+14,expected_data,data_size);
        if (!is_put || scenario==PUT_GET_MISSING) {
            *actual=14; p32(resp+4,UINT32_MAX); p32(resp+8,2);
        }
        if (scenario==PUT_GET_WRONG_DATA) resp[14]^=0x55;
        if (scenario==PUT_GET_WRONG_LENGTH) { *actual=13+data_size; p32(resp+4,(uint32_t)data_size-1); }
        if (scenario==PUT_GET_LONG_LENGTH) { *actual=15+data_size; p32(resp+4,(uint32_t)data_size+1); }
        if (scenario==PUT_GET_ZERO_LENGTH) { *actual=14; p32(resp+4,0); }
        if (scenario==PUT_GET_BAD_SEQUENCE) resp[12]^=1;
        if (scenario==PUT_GET_BAD_FRAME) *actual=14;
        if (scenario==PUT_GET_TRANSPORT || scenario==UNLINK_GET_FAIL) {
            (void)snprintf(err,errcap,"mock_receive_timeout:errno=110");
            return -1;
        }
        if (scenario==UNLINK_GET_PRESENT) { *actual=14+data_size; p32(resp+4,(uint32_t)data_size); p32(resp+8,0); }
        if (scenario==UNLINK_GET_DENIED) p32(resp+8,13);
    }
    return 0;
}
int main(void)
{
    unsigned int tests=0;
    uint32_t remote; char error[512];
    for (item_index=0; item_index<2; ++item_index) {
      size_t data_size=item_index==0?4u:36u;
      memset(expected_data,0,sizeof expected_data);
      if (item_index==0) {
          expected_data[0]=0x72; expected_data[1]=6; expected_data[2]=0xf1;
      } else {
          expected_data[0]=1; expected_data[20]=0x72; expected_data[21]=6; expected_data[24]=0xf1;
      }
    for (scenario=PUT_OK; scenario<=UNLINK_BAD_LENGTH; scenario=(enum scenario)(scenario+1)) {
        int result, success, early;
        calls=0; remote=UINT32_MAX; error[0]='\0';
        result=scenario<UNLINK_OK ? item_put(item_index,expected_data,data_size,&remote,error,sizeof error)
                                 : item_unlink(item_index,&remote,error,sizeof error);
        success=scenario==PUT_OK || scenario==PUT_OPAQUE_RECEIPT ||
                scenario==UNLINK_OK || scenario==UNLINK_ALREADY_ABSENT;
        early=(scenario>=PUT_SEND_FAIL && scenario<=PUT_OVERSIZE_RECEIPT) ||
              scenario==UNLINK_DENIED || scenario>=UNLINK_SEND_FAIL;
        assert(result==(success?0:-1));
        assert(calls==(early?1u:2u));
        if (success) assert(remote==(scenario==UNLINK_ALREADY_ABSENT?2u:0u));
        if (!success || scenario<UNLINK_OK) assert(error[0]!='\0');
        if (scenario==PUT_OPAQUE_RECEIPT) assert(strstr(error,"verified_by_get")!=NULL);
        if (scenario>=PUT_GET_WRONG_LENGTH && scenario<=PUT_GET_ZERO_LENGTH) {
            assert(strstr(error,"item_size_mismatch")!=NULL);
            assert(remote==UINT32_MAX);
        }
        if (scenario==PUT_GET_TRANSPORT || scenario==UNLINK_GET_FAIL)
            assert(strstr(error,"transport:mock_receive_timeout:errno=110")!=NULL);
        if (scenario==PUT_SEND_FAIL || scenario==UNLINK_SEND_FAIL)
            assert(strstr(error,"state_unknown:no_retry:mock_send_failure:errno=5")!=NULL);
        ++tests;
    }
    }
    calls=0;
    assert(item_put(ALL_LOCK_ITEM_COUNT,expected_data,36,&remote,error,sizeof error)==-1);
    assert(item_put(1,NULL,36,&remote,error,sizeof error)==-1);
    assert(item_put(1,expected_data,0,&remote,error,sizeof error)==-1);
    assert(item_put(1,expected_data,257,&remote,error,sizeof error)==-1);
    assert(item_put(1,expected_data,36,NULL,error,sizeof error)==-1);
    assert(item_unlink(ALL_LOCK_ITEM_COUNT,&remote,error,sizeof error)==-1);
    assert(item_unlink(1,NULL,error,sizeof error)==-1);
    assert(calls==0);
    printf("item_mutation: %u scenarios plus argument bounds passed\n",tests);
    return 0;
}
