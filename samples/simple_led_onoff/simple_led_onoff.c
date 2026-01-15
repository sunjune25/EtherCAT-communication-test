/** \file
 * \brief Simple Open EtherCAT master example with keyboard LED control
 *
 * Usage: simple_led_kbd IFNAME1
 * IFNAME1 is the NIC interface name, e.g. 'eth0'
 *
 * Keys:
 *   0 : both OFF
 *   1 : LED0 ON
 *   2 : LED1 ON
 *   3 : both ON
 *   q : quit
 */

#include "soem/soem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <termios.h>

typedef struct
{
   ecx_contextt context;
   char *iface;
   uint8 group;
   int roundtrip_time;
   uint8 map[4096];
} Fieldbus;

/* ---------- keyboard (non-blocking, single-char) ---------- */

static struct termios g_old_termios;
static int g_termios_saved = 0;

static void kb_enable_raw_mode(void)
{
   struct termios newt;
   if (tcgetattr(STDIN_FILENO, &g_old_termios) == 0)
   {
      g_termios_saved = 1;
      newt = g_old_termios;
      newt.c_lflag &= (tcflag_t) ~(ICANON | ECHO); /* non-canonical, no echo */
      newt.c_cc[VMIN] = 0;
      newt.c_cc[VTIME] = 0;
      tcsetattr(STDIN_FILENO, TCSANOW, &newt);
   }
}

static void kb_restore_mode(void)
{
   if (g_termios_saved)
   {
      tcsetattr(STDIN_FILENO, TCSANOW, &g_old_termios);
      g_termios_saved = 0;
   }
}

/* return 1 if a char is available, and store it in *c */
static int kb_try_read_char(char *c)
{
   fd_set rfds;
   struct timeval tv;
   int ret;

   FD_ZERO(&rfds);
   FD_SET(STDIN_FILENO, &rfds);
   tv.tv_sec = 0;
   tv.tv_usec = 0; /* no wait */

   ret = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
   if (ret > 0 && FD_ISSET(STDIN_FILENO, &rfds))
   {
      char ch;
      ssize_t n = read(STDIN_FILENO, &ch, 1);
      if (n == 1)
      {
         *c = ch;
         return 1;
      }
   }
   return 0;
}

/* ---------- fieldbus ---------- */

static void
fieldbus_initialize(Fieldbus *fieldbus, char *iface)
{
   memset(fieldbus, 0, sizeof(*fieldbus));
   fieldbus->iface = iface;
   fieldbus->group = 0;
   fieldbus->roundtrip_time = 0;
}

static int
fieldbus_roundtrip(Fieldbus *fieldbus)
{
   ecx_contextt *context;
   ec_timet start, end, diff;
   int wkc;

   context = &fieldbus->context;

   start = osal_current_time();
   ecx_send_processdata(context);
   wkc = ecx_receive_processdata(context, EC_TIMEOUTRET);
   end = osal_current_time();
   osal_time_diff(&start, &end, &diff);
   fieldbus->roundtrip_time = (int)(diff.tv_sec * 1000000 + diff.tv_nsec / 1000);

   return wkc;
}

static void
fieldbus_set_led_mask(Fieldbus *fieldbus, uint8 mask)
{
   ecx_contextt *context = &fieldbus->context;
   ec_groupt *grp = context->grouplist + fieldbus->group;

   if (grp->Obytes <= 0 || grp->outputs == NULL)
      return;

   /* Byte0: bit0=LED0, bit1=LED1 (assumption based on your slave code) */
   /* Keep other bits in Byte0 as-is, only touch bit0/bit1 */
   grp->outputs[0] &= (uint8)~0x03;
   grp->outputs[0] |= (mask & 0x03);
}

static boolean
fieldbus_start(Fieldbus *fieldbus)
{
   ecx_contextt *context;
   ec_groupt *grp;
   ec_slavet *slave;
   int i;

   context = &fieldbus->context;
   grp = context->grouplist + fieldbus->group;

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
   printf("mapped %dO+%dI bytes from %d segments\n",
          grp->Obytes, grp->Ibytes, grp->nsegments);

   printf("Configuring distributed clock... ");
   ecx_configdc(context);
   printf("done\n");

   printf("Waiting for all slaves in safe operational... ");
   ecx_statecheck(context, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);
   printf("done\n");

   /* start with LEDs OFF */
   fieldbus_set_led_mask(fieldbus, 0);

   printf("Send a roundtrip to make outputs in slaves happy... ");
   fieldbus_roundtrip(fieldbus);
   printf("done\n");

   printf("Setting operational state..");
   slave = context->slavelist; /* slave 0 (broadcast) */
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

   printf(" failed,");
   ecx_readstate(context);
   for (i = 1; i <= context->slavecount; ++i)
   {
      slave = context->slavelist + i;
      if (slave->state != EC_STATE_OPERATIONAL)
      {
         printf(" slave %d is 0x%04X (AL-status=0x%04X %s)",
                i, slave->state, slave->ALstatuscode,
                ec_ALstatuscode2string(slave->ALstatuscode));
      }
   }
   printf("\n");
   return FALSE;
}

static void
fieldbus_stop(Fieldbus *fieldbus)
{
   ecx_contextt *context;
   ec_slavet *slave;

   context = &fieldbus->context;
   slave = context->slavelist; /* slave 0 (broadcast) */

   printf("Requesting init state on all slaves... ");
   slave->state = EC_STATE_INIT;
   ecx_writestate(context, 0);
   printf("done\n");

   printf("Close socket... ");
   ecx_close(context);
   printf("done\n");
}

static boolean
fieldbus_dump(Fieldbus *fieldbus)
{
   ecx_contextt *context;
   ec_groupt *grp;
   uint32 n;
   int wkc, expected_wkc;

   context = &fieldbus->context;
   grp = context->grouplist + fieldbus->group;

   wkc = fieldbus_roundtrip(fieldbus);
   expected_wkc = grp->outputsWKC * 2 + grp->inputsWKC;

   printf("%6d usec  WKC %d", fieldbus->roundtrip_time, wkc);
   if (wkc < expected_wkc)
   {
      printf(" wrong (expected %d)\n", expected_wkc);
      return FALSE;
   }

   printf("  O:");
   for (n = 0; n < grp->Obytes; ++n) printf(" %02X", grp->outputs[n]);
   printf("  I:");
   for (n = 0; n < grp->Ibytes; ++n) printf(" %02X", grp->inputs[n]);
   printf("  T: %lld\r", (long long)context->DCtime);
   fflush(stdout);
   return TRUE;
}

static void
fieldbus_check_state(Fieldbus *fieldbus)
{
   ecx_contextt *context;
   ec_groupt *grp;
   ec_slavet *slave;
   int i;

   context = &fieldbus->context;
   grp = context->grouplist + fieldbus->group;

   grp->docheckstate = FALSE;
   ecx_readstate(context);

   for (i = 1; i <= context->slavecount; ++i)
   {
      slave = context->slavelist + i;
      if (slave->group != fieldbus->group)
      {
         /* other group */
      }
      else if (slave->state != EC_STATE_OPERATIONAL)
      {
         grp->docheckstate = TRUE;
         if (slave->state == EC_STATE_SAFE_OP + EC_STATE_ERROR)
         {
            printf("* Slave %d is in SAFE_OP+ERROR, attempting ACK\n", i);
            slave->state = EC_STATE_SAFE_OP + EC_STATE_ACK;
            ecx_writestate(context, i);
         }
         else if (slave->state == EC_STATE_SAFE_OP)
         {
            printf("* Slave %d is in SAFE_OP, change to OPERATIONAL\n", i);
            slave->state = EC_STATE_OPERATIONAL;
            ecx_writestate(context, i);
         }
         else if (slave->state > EC_STATE_NONE)
         {
            if (ecx_reconfig_slave(context, i, EC_TIMEOUTRET))
            {
               slave->islost = FALSE;
               printf("* Slave %d reconfigured\n", i);
            }
         }
         else if (!slave->islost)
         {
            ecx_statecheck(context, i, EC_STATE_OPERATIONAL, EC_TIMEOUTRET);
            if (slave->state == EC_STATE_NONE)
            {
               slave->islost = TRUE;
               printf("* Slave %d lost\n", i);
            }
         }
      }
      else if (slave->islost)
      {
         if (slave->state != EC_STATE_NONE)
         {
            slave->islost = FALSE;
            printf("* Slave %d found\n", i);
         }
         else if (ecx_recover_slave(context, i, EC_TIMEOUTRET))
         {
            slave->islost = FALSE;
            printf("* Slave %d recovered\n", i);
         }
      }
   }

   if (!grp->docheckstate)
      printf("All slaves resumed OPERATIONAL\n");
}

int main(int argc, char *argv[])
{
   Fieldbus fieldbus;

   if (argc != 2)
   {
      ec_adaptert *adapter = NULL;
      ec_adaptert *head = NULL;
      printf("Usage: simple_led_kbd IFNAME1\n"
             "IFNAME1 is the NIC interface name, e.g. 'eth0'\n");

      printf("\nAvailable adapters:\n");
      head = adapter = ec_find_adapters();
      while (adapter != NULL)
      {
         printf("    - %s  (%s)\n", adapter->name, adapter->desc);
         adapter = adapter->next;
      }
      ec_free_adapters(head);
      return 1;
   }

   fieldbus_initialize(&fieldbus, argv[1]);

   kb_enable_raw_mode();
   atexit(kb_restore_mode);

   if (fieldbus_start(&fieldbus))
   {
      int i, min_time, max_time;
      uint8 led_mode = 0;

      printf("\nKeys: 0=OFF, 1=LED0, 2=LED1, 3=BOTH, q=quit\n");

      min_time = max_time = 0;
      for (i = 1; i <= 1000000; ++i)
      {
         char ch;
         if (kb_try_read_char(&ch))
         {
            if (ch == 'q' || ch == 'Q')
               break;

            if (ch >= '0' && ch <= '3')
            {
               led_mode = (uint8)(ch - '0');
               printf("\n[LED mode set to %u]\n", led_mode);
            }
         }

         fieldbus_set_led_mask(&fieldbus, led_mode);

         printf("Iteration %6d:", i);
         if (!fieldbus_dump(&fieldbus))
            fieldbus_check_state(&fieldbus);
         else if (i == 1)
            min_time = max_time = fieldbus.roundtrip_time;
         else if (fieldbus.roundtrip_time < min_time)
            min_time = fieldbus.roundtrip_time;
         else if (fieldbus.roundtrip_time > max_time)
            max_time = fieldbus.roundtrip_time;

         osal_usleep(5000);
      }

      printf("\nRoundtrip time (usec): min %d max %d\n", min_time, max_time);
      fieldbus_stop(&fieldbus);
   }

   return 0;
}
