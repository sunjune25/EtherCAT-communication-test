#include "soem/soem.h"

#include <gpiod.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

/* ---------------- time helper ---------------- */
static long long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + (ts.tv_nsec / 1000LL);
}

/* ---------------- Fieldbus (SOEM) ---------------- */
typedef struct
{
   ecx_contextt context;
   char *iface;
   uint8 group;
   int roundtrip_time;
   uint8 map[4096];
} Fieldbus;

/* PDO offsets (질문에 적은 그대로) */
enum { OUT_LED = 2 };
enum { IN_A2_L = 0, IN_A2_H = 1 };

static void fieldbus_initialize(Fieldbus *fieldbus, char *iface)
{
   memset(fieldbus, 0, sizeof(*fieldbus));
   fieldbus->iface = iface;
   fieldbus->group = 0;
   fieldbus->roundtrip_time = 0;
}

static int fieldbus_roundtrip(Fieldbus *fieldbus)
{
   ecx_contextt *context = &fieldbus->context;
   ec_timet start, end, diff;
   int wkc;

   start = osal_current_time();
   ecx_send_processdata(context);
   wkc = ecx_receive_processdata(context, EC_TIMEOUTRET);
   end = osal_current_time();
   osal_time_diff(&start, &end, &diff);
   fieldbus->roundtrip_time = (int)(diff.tv_sec * 1000000 + diff.tv_nsec / 1000);

   return wkc;
}

static boolean fieldbus_start(Fieldbus *fieldbus)
{
   ecx_contextt *context = &fieldbus->context;
   ec_groupt *grp = context->grouplist + fieldbus->group;
   ec_slavet *slave;
   int i;

   printf("Initializing SOEM on '%s'... ", fieldbus->iface);
   if (!ecx_init(context, fieldbus->iface))
   {
      printf("no socket connection\n");
      return FALSE;
   }
   printf("done\n");

   printf("Finding autoconfig slaves... ");
   if (ecx_config_init(context) <= 0)
   {
      printf("no slaves found\n");
      return FALSE;
   }
   printf("%d slaves found\n", context->slavecount);

   printf("Sequential mapping of I/O... ");
   ecx_config_map_group(context, fieldbus->map, fieldbus->group);
   printf("mapped %dO+%dI bytes\n", grp->Obytes, grp->Ibytes);

   printf("Configuring distributed clock... ");
   ecx_configdc(context);
   printf("done\n");

   printf("Waiting for all slaves in safe operational... ");
   ecx_statecheck(context, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);
   printf("done\n");

   if (grp->outputs && grp->Obytes > 0)
      memset(grp->outputs, 0, grp->Obytes);

   printf("Send a roundtrip... ");
   fieldbus_roundtrip(fieldbus);
   printf("done\n");

   printf("Setting operational state..");
   slave = context->slavelist; /* slave 0 broadcast */
   slave->state = EC_STATE_OPERATIONAL;
   ecx_writestate(context, 0);

   for (i = 0; i < 10; ++i)
   {
      printf(".");
      fieldbus_roundtrip(fieldbus);
      ecx_statecheck(context, 0, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE / 10);
      if (slave->state == EC_STATE_OPERATIONAL)
      {
         printf(" all slaves are now operational\n");
         return TRUE;
      }
   }

   printf(" failed\n");
   return FALSE;
}

static void fieldbus_stop(Fieldbus *fieldbus)
{
   ecx_contextt *context = &fieldbus->context;
   ec_slavet *slave = context->slavelist;

   printf("Requesting init state on all slaves... ");
   slave->state = EC_STATE_INIT;
   ecx_writestate(context, 0);
   printf("done\n");

   printf("Close socket... ");
   ecx_close(context);
   printf("done\n");
}

static void fieldbus_check_state(Fieldbus *fieldbus)
{
   ecx_contextt *context = &fieldbus->context;
   ec_groupt *grp = context->grouplist + fieldbus->group;
   ec_slavet *slave;
   int i;

   grp->docheckstate = FALSE;
   ecx_readstate(context);

   for (i = 1; i <= context->slavecount; ++i)
   {
      slave = context->slavelist + i;
      if (slave->group != fieldbus->group) continue;

      if (slave->state != EC_STATE_OPERATIONAL)
      {
         grp->docheckstate = TRUE;

         if (slave->state == EC_STATE_SAFE_OP + EC_STATE_ERROR)
         {
            slave->state = EC_STATE_SAFE_OP + EC_STATE_ACK;
            ecx_writestate(context, i);
         }
         else if (slave->state == EC_STATE_SAFE_OP)
         {
            slave->state = EC_STATE_OPERATIONAL;
            ecx_writestate(context, i);
         }
         else if (slave->state > EC_STATE_NONE)
         {
            ecx_reconfig_slave(context, i, EC_TIMEOUTRET);
         }
         else
         {
            ecx_statecheck(context, i, EC_STATE_OPERATIONAL, EC_TIMEOUTRET);
         }
      }
   }
}

static void set_led_output(Fieldbus *fieldbus, uint8_t led_0_to_3)
{
   ecx_contextt *context = &fieldbus->context;
   ec_groupt *grp = context->grouplist + fieldbus->group;

   if (grp->outputs && grp->Obytes > OUT_LED)
      grp->outputs[OUT_LED] = (uint8_t)(led_0_to_3 & 0x03);
}

static int read_analoginput2(Fieldbus *fieldbus, uint16_t *out_value)
{
   ecx_contextt *context = &fieldbus->context;
   ec_groupt *grp = context->grouplist + fieldbus->group;

   if (!grp->inputs || grp->Ibytes < 2) return 0;

   *out_value = (uint16_t)grp->inputs[IN_A2_L] |
                ((uint16_t)grp->inputs[IN_A2_H] << 8);
   return 1;
}

/* ---------------- GPIO ISR-like thread ---------------- */
struct gpio_isr_ctx {
    struct gpiod_line *in_line;

    atomic_int running;
    atomic_int event_flag;   /* 0/1 : falling edge 발생하면 1 */
    long long debounce_us;
    long long last_us;
};

static void* gpio_falling_thread(void *arg)
{
    struct gpio_isr_ctx *c = (struct gpio_isr_ctx*)arg;

    while (atomic_load(&c->running)) {
        struct timespec to;
        to.tv_sec = 0;
        to.tv_nsec = 200 * 1000 * 1000; /* 200ms */

        int ret = gpiod_line_event_wait(c->in_line, &to);
        if (ret < 0) {
            fprintf(stderr, "gpiod_line_event_wait error: %s\n", strerror(errno));
            break;
        }
        if (ret == 0) continue;

        struct gpiod_line_event ev;
        if (gpiod_line_event_read(c->in_line, &ev) < 0) {
            fprintf(stderr, "gpiod_line_event_read error: %s\n", strerror(errno));
            break;
        }

        if (ev.event_type == GPIOD_LINE_EVENT_FALLING_EDGE) {
            long long t = now_us();
            if (t - c->last_us < c->debounce_us) continue;
            c->last_us = t;

            /* ISR에서 하는 일: 이벤트 플래그만 1로 */
            atomic_store(&c->event_flag, 1);
        }
    }

    atomic_store(&c->running, 0);
    return NULL;
}

/* ---------------- graceful stop ---------------- */
static atomic_int g_run = 1;
static void on_signal(int sig)
{
    (void)sig;
    atomic_store(&g_run, 0);
}

/* ---------------- main ---------------- */
int main(int argc, char *argv[])
{
    if (argc != 2) {
        printf("Usage: app IFNAME1\n");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* ---- GPIO setup ---- */
    const char *chip_path = "/dev/gpiochip0";
    const unsigned int in_offset  = 106; /* input falling edge */
    const unsigned int out_offset = 43;  /* output toggle */

    struct gpiod_chip *chip = gpiod_chip_open(chip_path);
    if (!chip) {
        fprintf(stderr, "gpiod_chip_open(%s) failed: %s\n", chip_path, strerror(errno));
        return 1;
    }

    struct gpiod_line *in_line = gpiod_chip_get_line(chip, in_offset);
    struct gpiod_line *out_line = gpiod_chip_get_line(chip, out_offset);
    if (!in_line || !out_line) {
        fprintf(stderr, "gpiod_chip_get_line failed: %s\n", strerror(errno));
        gpiod_chip_close(chip);
        return 1;
    }

    int gpio_out_state = 0;
    if (gpiod_line_request_output(out_line, "gpio-out-toggle", gpio_out_state) < 0) {
        fprintf(stderr, "gpiod_line_request_output failed: %s\n", strerror(errno));
        gpiod_chip_close(chip);
        return 1;
    }

    if (gpiod_line_request_falling_edge_events(in_line, "gpio-in-falling") < 0) {
        fprintf(stderr, "gpiod_line_request_falling_edge_events failed: %s\n", strerror(errno));
        gpiod_line_release(out_line);
        gpiod_chip_close(chip);
        return 1;
    }

    printf("GPIO input  falling: %s offset %u\n", chip_path, in_offset);
    printf("GPIO output toggle : %s offset %u (initial=%d)\n", chip_path, out_offset, gpio_out_state);

    /* ---- EtherCAT setup ---- */
    Fieldbus fieldbus;
    fieldbus_initialize(&fieldbus, argv[1]);
    if (!fieldbus_start(&fieldbus)) {
        gpiod_line_release(in_line);
        gpiod_line_release(out_line);
        gpiod_chip_close(chip);
        return 1;
    }

    /* ---- GPIO ISR-like thread start ---- */
    struct gpio_isr_ctx isr;
    memset(&isr, 0, sizeof(isr));
    isr.in_line = in_line;
    isr.debounce_us = 200000; /* 200ms */
    isr.last_us = 0;
    atomic_store(&isr.running, 1);
    atomic_store(&isr.event_flag, 0);

    pthread_t th;
    if (pthread_create(&th, NULL, gpio_falling_thread, &isr) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        atomic_store(&isr.running, 0);
        fieldbus_stop(&fieldbus);
        gpiod_line_release(in_line);
        gpiod_line_release(out_line);
        gpiod_chip_close(chip);
        return 1;
    }

    /* ---- main loop ----
       EtherCAT은 주기적으로 roundtrip 유지(슬레이브 watchdog 때문)
       이벤트가 들어오면 해당 cycle에서 LED를 변경하고, 성공하면 GPIO out 토글 */
    uint8_t led = 0;
    int iter = 0;

    int toggle_after_send = 0; /* 이벤트 소비했으면 1로 세팅 -> 이번 송수신 성공하면 GPIO 토글 */

    printf("\nRunning... (Ctrl+C to quit)\n");

    while (atomic_load(&g_run) && atomic_load(&isr.running)) {
        iter++;

        /* 1) falling edge 이벤트 소비: 1이면 0으로 내리면서(consume) true */
        int got = atomic_exchange(&isr.event_flag, 0);
        if (got == 1) {
            /* 2) LED 패턴 0->1->2->3->0... */
            led = (uint8_t)((led + 1) & 0x03);
            set_led_output(&fieldbus, led);

            /* 3) 이번 송수신이 성공하면 GPIO 토글 */
            toggle_after_send = 1;
        }

        ecx_contextt *context = &fieldbus.context;
        ec_groupt *grp = context->grouplist + fieldbus.group;

        int wkc = fieldbus_roundtrip(&fieldbus);
        int expected_wkc = grp->outputsWKC * 2 + grp->inputsWKC;

        if (wkc < expected_wkc) {
            fieldbus_check_state(&fieldbus);
            toggle_after_send = 0; /* 실패했으면 이번 이벤트에 대한 토글은 보류/취소(원하면 유지로 바꿔도 됨) */
            osal_usleep(5000);
            continue;
        }

        if (toggle_after_send) {
            gpio_out_state = !gpio_out_state;
            if (gpiod_line_set_value(out_line, gpio_out_state) < 0) {
                fprintf(stderr, "gpiod_line_set_value failed: %s\n", strerror(errno));
                break;
            }
            toggle_after_send = 0;

            int rb = gpiod_line_get_value(out_line);
            printf("[EVT] LED=%u | GPIO_OUT set=%d readback=%d | wkc=%d/%d\n",
                   (unsigned)led, gpio_out_state, rb, wkc, expected_wkc);
        }

        /* (선택) 입력값도 보고 싶으면 */
        uint16_t analog2 = 0;
        if (read_analoginput2(&fieldbus, &analog2)) {
            /* 너무 출력 많으면 주석 처리 */
            /* printf("Iter %d | LED=%u | AnalogInput2=%u\n", iter, (unsigned)led, (unsigned)analog2); */
            (void)analog2;
        }

        osal_usleep(5000); /* 5ms 주기 */
    }

    printf("\nStopping...\n");

    /* stop thread */
    atomic_store(&isr.running, 0);
    pthread_join(th, NULL);

    /* cleanup */
    fieldbus_stop(&fieldbus);

    gpiod_line_release(in_line);
    gpiod_line_release(out_line);
    gpiod_chip_close(chip);

    return 0;
}
