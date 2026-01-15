/**
 * read_light_led.c
 *
 * - 키입력으로 Output PDO의 LED_Output(Byte[2])만 0~3으로 변경
 * - 매 iteration마다 Iteration, LED_Output, AnalogInput2(입력 PDO Byte[0..1])만 출력
 *
 * 키:
 *   '0'~'3' : LED_Output = 0~3
 *   '+'     : LED_Output 증가 (0~3 wrap)
 *   '-'     : LED_Output 감소 (0~3 wrap)
 *   'q'     : 종료
 */

#include "soem/soem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <unistd.h>
#include <sys/select.h>
#include <termios.h>
#include <signal.h>

typedef struct
{
   ecx_contextt context;
   char *iface;
   uint8 group;
   int roundtrip_time;
   uint8 map[4096];
} Fieldbus;

/* --- PDO offsets (너가 준 헤더 기준) ---
 * Output (Master->Slave):
 *   Byte[0] TXD_VALUE_RS232
 *   Byte[1] TXD_VALUE_RS485
 *   Byte[2] LED_Output        <-- 우리가 변경
 *   Byte[3] FND_Output
 *   Byte[4] BUZZ_Output
 *
 * Input (Slave->Master):
 *   Byte[0..1] AnalogInput2   <-- 우리가 읽음 (uint16 little-endian)
 *   Byte[2..3] AnalogInput
 *   Byte[4] RXD_VALUE_RS232
 *   Byte[5] RXD_VALUE_RS485
 *   Byte[6] DIPSW_INPUT
 */
enum { OUT_LED = 2 };
enum { IN_A2_L = 0, IN_A2_H = 1 };

/* --------- non-blocking key input --------- */
static struct termios g_old_tio;
static int g_tio_saved = 0;

static void restore_terminal(void)
{
   if (g_tio_saved)
   {
      tcsetattr(STDIN_FILENO, TCSANOW, &g_old_tio);
      g_tio_saved = 0;
   }
}

static void on_signal(int sig)
{
   (void)sig;
   restore_terminal();
   _exit(1);
}

static void set_terminal_raw(void)
{
   struct termios new_tio;

   if (tcgetattr(STDIN_FILENO, &g_old_tio) == 0)
   {
      g_tio_saved = 1;
      atexit(restore_terminal);

      new_tio = g_old_tio;
      new_tio.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
      new_tio.c_cc[VMIN]  = 0;
      new_tio.c_cc[VTIME] = 0;
      tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);

      signal(SIGINT, on_signal);
      signal(SIGTERM, on_signal);
   }
}

static int read_key_nonblock(void)
{
   fd_set rfds;
   struct timeval tv;
   unsigned char c;

   FD_ZERO(&rfds);
   FD_SET(STDIN_FILENO, &rfds);
   tv.tv_sec = 0;
   tv.tv_usec = 0;

   int r = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
   if (r > 0 && FD_ISSET(STDIN_FILENO, &rfds))
   {
      if (read(STDIN_FILENO, &c, 1) == 1)
         return (int)c;
   }
   return -1;
}
/* ----------------------------------------- */

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

   /* outputs 0으로 초기화 (TXD/FND/BUZZ 포함) */
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
      grp->outputs[OUT_LED] = (uint8_t)(led_0_to_3 & 0x03); /* 0~3만 */
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

int main(int argc, char *argv[])
{
   Fieldbus fieldbus;

   if (argc != 2)
   {
      printf("Usage: read_light_led IFNAME1\n");
      return 1;
   }

   fieldbus_initialize(&fieldbus, argv[1]);
   if (!fieldbus_start(&fieldbus)) return 1;

   set_terminal_raw();
   printf("\nKeys: 0~3 set LED_Output, +/- change, q quit\n");

   uint8_t led = 0;
   int iter = 0;

   for (;;)
   {
      iter++;

      /* 키로 LED_Output만 변경 */
      int key = read_key_nonblock();
      if (key != -1)
      {
         if (key == 'q' || key == 'Q') break;
         if (key >= '0' && key <= '3') led = (uint8_t)(key - '0');
         else if (key == '+') led = (uint8_t)((led + 1) & 0x03);
         else if (key == '-') led = (uint8_t)((led + 3) & 0x03); /* -1 mod 4 */
      }

      set_led_output(&fieldbus, led);

      ecx_contextt *context = &fieldbus.context;
      ec_groupt *grp = context->grouplist + fieldbus.group;

      int wkc = fieldbus_roundtrip(&fieldbus);
      int expected_wkc = grp->outputsWKC * 2 + grp->inputsWKC;

      if (wkc < expected_wkc)
      {
         fieldbus_check_state(&fieldbus);
         osal_usleep(5000);
         continue;
      }

      uint16_t analog2 = 0;
      if (!read_analoginput2(&fieldbus, &analog2))
      {
         osal_usleep(5000);
         continue;
      }

      /* 매 iteration: LED_Output 값 + light sensor(AnalogInput2)만 출력 */
      printf("Iteration %6d | LED_Output=%u | AnalogInput2=%u\n",
             iter, (unsigned)led, (unsigned)analog2);

      osal_usleep(5000);
   }

   printf("\nStopping...\n");
   fieldbus_stop(&fieldbus);
   return 0;
}
