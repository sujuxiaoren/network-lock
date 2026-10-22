/* Host-only tests: every DIAG exchange is a scripted mock; no device access. */
#include "../native/efs.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { CMD_OPEN=2, CMD_CLOSE=3, CMD_READ=4, CMD_WRITE=5, CMD_STAT=15 };
enum fault {
    OK, REMOTE_ERROR, SHORT_PACKET, WRONG_CMD, WRONG_SUBSYSTEM, WRONG_SUBCOMMAND,
    BAD_FD, BAD_OFFSET, PARTIAL, EXTRA_BYTE, TRANSPORT_ERROR, BAD_MODE, BAD_SIZE,
    NEGATIVE_FD, CLAIM_OVERSIZED, ITEM_FILE, MISSING_FIELD, MISSING_DATA
};
struct step { unsigned int cmd; enum fault fault; };
static struct step script[8];
static size_t script_size, current_step;
static int write_operation, frequency_path, test_count;
static const unsigned char lock_value[4] = {0x34,0x12,0x56,0x01};
static const char pci_path[] = "/nv/item_files/modem/lte/rrc/csp/pci_lock";
static const char earfcn_path[] = "/nv/item_files/modem/lte/rrc/csp/earfcn_lock";

static uint32_t u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
           ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static void p32(unsigned char *p, uint32_t v)
{
    p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8);
    p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24);
}
static const char *active_path(void)
{
    return frequency_path ? earfcn_path : pci_path;
}

static void reset(int writing, int frequency)
{
    script_size=0; current_step=0; write_operation=writing; frequency_path=frequency;
}
static void add(unsigned int cmd, enum fault f)
{
    assert(script_size < sizeof script/sizeof script[0]);
    script[script_size].cmd=cmd; script[script_size++].fault=f;
}

int diag_exchange(const unsigned char *request, size_t req_len,
                  unsigned char *response, size_t response_cap,
                  size_t *response_len, char *err, size_t errlen)
{
    unsigned char answer[80]={0};
    size_t answer_len=0;
    struct step step;
    assert(current_step < script_size);
    step=script[current_step++];
    assert(request != NULL && req_len >= 4);
    assert(request[0]==0x4b && request[1]==0x13);
    assert(request[2]==step.cmd && request[3]==0);
    memcpy(answer, request, 4);
    switch (step.cmd) {
    case CMD_STAT:
        assert(req_len==4+strlen(active_path())+1);
        assert(memcmp(request+4,active_path(),strlen(active_path())+1)==0);
        answer_len=32;
        p32(answer+8,0100600u); p32(answer+12,4); p32(answer+16,1);
        break;
    case CMD_OPEN:
        assert(req_len==12+strlen(active_path())+1);
        /* This assertion rejects CREATE, TRUNCATE, and host-specific flags. */
        assert(u32(request+4)==(write_operation ? 2u : 0u));
        assert(u32(request+8)==0);
        assert(memcmp(request+12,active_path(),strlen(active_path())+1)==0);
        answer_len=12; p32(answer+4,7);
        break;
    case CMD_READ:
        assert(!write_operation && req_len==16);
        assert(u32(request+4)==7 && u32(request+8)==4 && u32(request+12)==0);
        answer_len=24; p32(answer+4,7); p32(answer+12,4);
        memcpy(answer+20,lock_value,4);
        break;
    case CMD_WRITE:
        assert(write_operation && req_len==16);
        assert(u32(request+4)==7 && u32(request+8)==0);
        assert(memcmp(request+12,lock_value,4)==0);
        answer_len=20; p32(answer+4,7); p32(answer+12,4);
        break;
    case CMD_CLOSE:
        assert(req_len==8 && u32(request+4)==7);
        answer_len=8; /* CLOSE has no fd echo in the protocol. */
        break;
    default:
        assert(0);
    }
    switch (step.fault) {
    case OK: break;
    case REMOTE_ERROR:
        if (step.cmd==CMD_STAT || step.cmd==CMD_CLOSE) p32(answer+4,13);
        else if (step.cmd==CMD_OPEN) p32(answer+8,2);
        else p32(answer+16,5);
        break;
    case SHORT_PACKET: answer_len=3; break;
    case WRONG_CMD: answer[0]=0x13; break;
    case WRONG_SUBSYSTEM: answer[1]=0x3e; break;
    case WRONG_SUBCOMMAND: answer[2]^=1; break;
    case BAD_FD: p32(answer+4,8); break;
    case BAD_OFFSET: p32(answer+8,1); break;
    case PARTIAL:
        p32(answer+12,2);
        if (step.cmd==CMD_READ) answer_len=22;
        break;
    case EXTRA_BYTE: answer[answer_len++]=0xaa; break;
    case TRANSPORT_ERROR:
        if (err != NULL && errlen != 0) (void)snprintf(err,errlen,"mock timeout");
        return -1;
    case BAD_MODE: p32(answer+8,0040700u); break;
    case ITEM_FILE: p32(answer+8,0160600u); break;
    case BAD_SIZE: p32(answer+12,8); break;
    case NEGATIVE_FD: p32(answer+4,UINT32_MAX); break;
    case MISSING_FIELD: answer_len=19; break;
    case MISSING_DATA: answer_len=20; break;
    case CLAIM_OVERSIZED:
        *response_len=response_cap+1;
        return 0;
    }
    assert(answer_len <= response_cap);
    memcpy(response,answer,answer_len);
    *response_len=answer_len;
    return 0;
}

static void check_call(int writing, int success, const char *expected_error)
{
    char error[320]={0};
    unsigned char output[4]={0xcc,0xcc,0xcc,0xcc};
    const unsigned char untouched[4]={0xcc,0xcc,0xcc,0xcc};
    int result=writing ? (frequency_path ?
                        efs_write_earfcn(lock_value,error,sizeof error) :
                        efs_write_lock(lock_value,error,sizeof error)) :
                        efs_read_lock(output,error,sizeof error);
    assert(current_step==script_size);
    if (success) {
        assert(result==0 && error[0]=='\0');
        if (!writing) assert(memcmp(output,lock_value,4)==0);
    } else {
        if (result==0 || strstr(error,expected_error)==NULL) {
            fprintf(stderr,"case %d result=%d error=%s expected=%s\n",
                    test_count,result,error,expected_error);
            assert(0);
        }
        if (!writing) assert(memcmp(output,untouched,4)==0);
    }
    ++test_count;
}

/* phase: 0=STAT, 1=OPEN, 2=READ/WRITE, 3=CLOSE, -1=success */
static void scenario_one(int writing, int frequency, int phase, enum fault fault,
                         const char *expected_error)
{
    reset(writing, frequency);
    add(CMD_STAT,phase==0 ? fault : OK);
    if (phase!=0) {
        add(CMD_OPEN,phase==1 ? fault : OK);
        if (phase!=1) {
            add(writing ? CMD_WRITE : CMD_READ,phase==2 ? fault : OK);
            add(CMD_CLOSE,phase==3 ? fault : OK);
        }
    }
    check_call(writing,phase==-1,expected_error);
}

static void scenario(int writing, int phase, enum fault fault,
                     const char *expected_error)
{
    unsigned int frequency;
    for (frequency = 0; frequency < (writing ? 2u : 1u); ++frequency)
        scenario_one(writing, (int)frequency, phase, fault, expected_error);
}

int main(void)
{
    char tiny_error[1];
    unsigned char output[4];
    scenario(0,-1,OK,"");
    scenario(1,-1,OK,"");
    scenario(0,0,REMOTE_ERROR,"STAT remote errno");
    scenario(1,0,BAD_MODE,"not a regular file");
    scenario(1,0,ITEM_FILE,"not a regular file");
    scenario(1,0,BAD_SIZE,"expected 4");
    scenario(0,0,SHORT_PACKET,"invalid response length");
    scenario(0,0,WRONG_CMD,"mismatch");
    scenario(0,0,WRONG_SUBSYSTEM,"mismatch");
    scenario(0,0,WRONG_SUBCOMMAND,"mismatch");
    scenario(0,0,EXTRA_BYTE,"response length");
    scenario(0,0,CLAIM_OVERSIZED,"invalid response length");
    scenario(1,1,REMOTE_ERROR,"OPEN remote errno");
    scenario(0,1,NEGATIVE_FD,"negative file descriptor");
    scenario(0,1,EXTRA_BYTE,"response length");
    scenario(0,1,TRANSPORT_ERROR,"mock timeout");
    scenario(0,2,REMOTE_ERROR,"READ remote errno");
    scenario(0,2,SHORT_PACKET,"invalid response length");
    scenario(0,2,MISSING_FIELD,"too short");
    scenario(0,2,MISSING_DATA,"response length");
    scenario(0,2,BAD_FD,"fd/offset mismatch");
    scenario(0,2,BAD_OFFSET,"fd/offset mismatch");
    scenario(0,2,PARTIAL,"partial/invalid count");
    scenario(0,2,EXTRA_BYTE,"response length");
    scenario(0,2,TRANSPORT_ERROR,"mock timeout");
    scenario(1,2,REMOTE_ERROR,"WRITE remote errno");
    scenario(1,2,SHORT_PACKET,"invalid response length");
    scenario(1,2,MISSING_FIELD,"too short");
    scenario(1,2,BAD_FD,"fd/offset mismatch");
    scenario(1,2,BAD_OFFSET,"fd/offset mismatch");
    scenario(1,2,PARTIAL,"partial/invalid count");
    scenario(1,2,EXTRA_BYTE,"response length");
    scenario(1,2,WRONG_SUBCOMMAND,"mismatch");
    scenario(1,2,TRANSPORT_ERROR,"mock timeout");
    scenario(0,3,REMOTE_ERROR,"CLOSE remote errno");
    scenario(1,3,REMOTE_ERROR,"CLOSE remote errno");
    scenario(1,3,SHORT_PACKET,"invalid response length");
    scenario(0,3,WRONG_SUBCOMMAND,"mismatch");
    scenario(1,3,EXTRA_BYTE,"response length");
    scenario(1,3,TRANSPORT_ERROR,"mock timeout");
    {
        unsigned int frequency;
        for (frequency = 0; frequency < 2; ++frequency) {
            reset(1, (int)frequency); add(CMD_STAT,OK); add(CMD_OPEN,OK);
            add(CMD_WRITE,PARTIAL); add(CMD_CLOSE,REMOTE_ERROR);
            check_call(1,0,"additionally EFS CLOSE");
        }
    }

    reset(0, 0);
    assert(efs_read_lock(NULL,tiny_error,sizeof tiny_error)==-1);
    assert(tiny_error[0]=='\0' && current_step==0);
    assert(efs_write_lock(NULL,NULL,0)==-1 && current_step==0);
    reset(1, 1);
    assert(efs_write_earfcn(NULL,NULL,0)==-1 && current_step==0);
    reset(0, 0);
    add(CMD_STAT,REMOTE_ERROR);
    assert(efs_read_lock(output,NULL,0)==-1 && current_step==1);
    printf("efs_test: %d scripted cases plus null/bounds checks passed\n",test_count);
    return 0;
}
