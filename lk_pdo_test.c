#include <stdio.h>
#include <string.h>
#include <signal.h>


#include "soem/soem.h"   // ecx_* 함수, ecx_contextt, 슬레이브/그룹 구조체 등


#define EC_TIMEOUTMON 500


// 전역 버퍼 & 상태 값
static char IOmap[4096];
static volatile int stop = 0;


static void sigint_handler(int sig)
{
    (void)sig;
    stop = 1;
}


int main(int argc, char *argv[])
{
    const char *ifname = "enP8p1s0";   // 네가 쓰는 NIC 이름 (필요하면 바꾸고, argv[1]로 받아도 됨)
    if (argc > 1)
        ifname = argv[1];


    printf("==== SOEM lk_pdo_test (ecx_* API) ====\n");
    printf("Interface : %s\n", ifname);


    // 컨텍스트 & IOmap 초기화
    ecx_contextt context;
    memset(&context, 0, sizeof(context));
    memset(IOmap, 0, sizeof(IOmap));


    // Ctrl+C 핸들러
    signal(SIGINT, sigint_handler);


    // 1) EtherCAT master 초기화
    if (!ecx_init(&context, ifname)) {
        printf("ecx_init() failed on %s\n", ifname);
        return -1;
    }
    printf("ecx_init() OK\n");


    // 2) 슬레이브 스캔 & 초기 설정
    int slave_count = ecx_config_init(&context);
    if (slave_count <= 0) {
        printf("No slaves found!\n");
        ecx_close(&context);
        return -1;
    }
    printf("Found %d slaves\n", slave_count);


    // 3) PDO 매핑 & DC 설정
    int iomap_size = ecx_config_map_group(&context, IOmap, 0);
    printf("IOmap size: %d bytes\n", iomap_size);


    ecx_configdc(&context);

 printf("Group0 outputsWKC=%d, inputsWKC=%d\n", 
           context.grouplist[0].outputsWKC, 
           context.grouplist[0].inputsWKC);

    // 슬레이브가 1개라고 가정 (첫 번째 슬레이브는 인덱스 1)
    printf("Slave1 Obits=%d, Ibits=%d\n",
           context.slavelist[1].Obits,
           context.slavelist[1].Ibits);

    // 4) SAFE_OP 진입
    context.slavelist[0].state = EC_STATE_SAFE_OP;
    ecx_writestate(&context, 0);
    ecx_statecheck(&context, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE);
    printf("Master state after SAFE_OP request: 0x%02X\n", context.slavelist[0].state);


    // 5) OP 진입 요청
    context.slavelist[0].state = EC_STATE_OPERATIONAL;
    ecx_writestate(&context, 0);


    int chk = 40;
    int expectedWKC;
    int wkc;


    // expectedWKC 계산 (컨텍스트 기반)
    expectedWKC = (context.grouplist[0].outputsWKC * 2) + context.grouplist[0].inputsWKC;
    printf("expectedWKC = %d\n", expectedWKC);


    // OP 상태 될 때까지 체크
    do {
        ecx_send_processdata(&context);
        wkc = ecx_receive_processdata(&context, EC_TIMEOUTRET);
        ecx_statecheck(&context, 0, EC_STATE_OPERATIONAL, EC_TIMEOUTRET);
    } while (chk-- && (context.slavelist[0].state != EC_STATE_OPERATIONAL));


    if (context.slavelist[0].state != EC_STATE_OPERATIONAL) {
        printf("Not all slaves reached OP state, current master state: 0x%02X\n",
               context.slavelist[0].state);
        ecx_close(&context);
        return -1;
    }


    printf("All slaves in OP state, start cyclic PDO exchange.\n");
    printf("Press Ctrl+C to stop.\n");


    // 6) 주기 통신 루프
    int cycle = 0;
    while (!stop) {
        ecx_send_processdata(&context);
        wkc = ecx_receive_processdata(&context, EC_TIMEOUTRET);


        if (wkc >= expectedWKC) {
            // 예시: 첫 번째 슬레이브의 I/O 1바이트만 보기
            if (slave_count >= 1) {
                uint8 *outputs = context.slavelist[1].outputs;
                uint8 *inputs  = context.slavelist[1].inputs;


                uint8 out0 = outputs ? outputs[0] : 0;
                uint8 in0  = inputs  ? inputs[0]  : 0;


                // 간단히 토글 예시 (출력 비트 0 토글)
                if (outputs) {
                    if ((cycle % 100) == 0) { // 너무 자주 안 바꾸게 예시
                        out0 ^= 0x01;
                        outputs[0] = out0;
                    }
                }


                printf("\rCycle %6d | WKC=%3d | OUT0=0x%02X | IN0=0x%02X",
                       cycle, wkc, out0, in0);
                fflush(stdout);
            } else {
                printf("\rCycle %6d | WKC=%3d", cycle, wkc);
                fflush(stdout);
            }
        } else {
            printf("\rCycle %6d | WKC too low (%d / %d)", cycle, wkc, expectedWKC);
            fflush(stdout);
        }


        cycle++;
        osal_usleep(10000); // 10ms 주기 예시
    }


    printf("\nStopping, set state INIT.\n");
    context.slavelist[0].state = EC_STATE_INIT;
    ecx_writestate(&context, 0);
    ecx_close(&context);


    return 0;
}

