/* SPDX-License-Identifier: BSD-2-Clause */

#include <tilck/common/basic_defs.h>
#include <tilck/common/printk.h>
#include <tilck/common/atomics.h>
#include <tilck/common/utils.h>

#include <tilck/kernel/tasklet.h>
#include <tilck/kernel/sync.h>
#include <tilck/kernel/sched.h>
#include <tilck/kernel/debug_utils.h>
#include <tilck/kernel/self_tests.h>

static ATOMIC(u32) g_counter;
static u64 g_cycles_begin;

struct se_tasklet_ctx {
   struct kmutex m;
   struct kcond c;
};

static void test_tasklet_func(void *arg)
{
   g_counter++;
}

static void end_test(void *arg)
{
   struct se_tasklet_ctx *ctx = arg;

   const u32 max_tasklets = get_worker_queue_size(0);
   const u32 tot_iters = max_tasklets * 10;

   u64 elapsed = RDTSC() - g_cycles_begin;
   VERIFY(g_counter == tot_iters);
   printk("[se_tasklet] Avg cycles per tasklet "
          "(enqueue + execute): %llu\n", elapsed/g_counter);

   kmutex_lock(&ctx->m);
   {
      printk("[se_tasklet] end_test() holding the lock and signalling cond\n");
      kcond_signal_one(&ctx->c);
   }
   kmutex_unlock(&ctx->m);
   printk("[se_tasklet] end_test() func completed\n");
}

void selftest_tasklet_short(void)
{
   const u32 max_tasklets = get_worker_queue_size(0);
   const u32 tot_iters = max_tasklets * 10;
   const u32 attempts_check = 500 * 1000;
   struct se_tasklet_ctx ctx;
   u32 yields_count = 0;
   u64 tot_attempts = 0;
   u32 last_counter_val;
   u32 counter_now;
   bool added;

   kcond_init(&ctx.c);
   kmutex_init(&ctx.m, 0);
   g_counter = 0;

   ASSERT(is_preemption_enabled());
   printk("[se_tasklet] BEGIN\n");

   g_cycles_begin = RDTSC();

   for (u32 i = 0; i < tot_iters; i++) {

      u32 attempts = 1;
      last_counter_val = g_counter;
      bool did_yield = false;

      do {

         added = enqueue_job(0, &test_tasklet_func, NULL);
         attempts++;

         if (!(attempts % attempts_check)) {

            counter_now = g_counter;

            if (counter_now == last_counter_val) {

               if (did_yield)
                  panic("It seems that tasklets don't get executed");

               did_yield = true;
               yields_count++;
               kernel_yield();
            }
         }

      } while (!added);

      tot_attempts += attempts;
   }

   last_counter_val = g_counter;
   printk("[se_tasklet] Main test done\n");
   printk("[se_tasklet] AVG attempts: %u\n", (u32)(tot_attempts/tot_iters));
   printk("[se_tasklet] Yields:       %u\n", yields_count);
   printk("[se_tasklet] counter now:  %u\n", last_counter_val);
   printk("[se_tasklet] now wait for completion...\n");
   kernel_sleep(1);

   do {

      counter_now = g_counter;

      if (counter_now == last_counter_val)
         panic("It seems that tasklets don't get executed");

      last_counter_val = counter_now;
      kernel_sleep(1);

   } while (counter_now < tot_iters);

   printk("[se_tasklet] DONE, counter: %u\n", counter_now);
   printk("[se_tasklet] enqueue end_test()\n");
   kmutex_lock(&ctx.m);
   {
      printk("[se_tasklet] Under lock, before enqueue\n");

      do {
         added = enqueue_job(0, &end_test, &ctx);
      } while (!added);

      printk("[se_tasklet] Under lock, AFTER enqueue\n");
      printk("[se_tasklet] Now, wait on cond\n");
      kcond_wait(&ctx.c, &ctx.m, KCOND_WAIT_FOREVER);
   }
   kmutex_unlock(&ctx.m);
   kcond_destory(&ctx.c);
   kmutex_destroy(&ctx.m);
   printk("[se_tasklet] END\n");
   regular_self_test_end();
}

void selftest_tasklet_perf_short(void)
{
   bool added;
   u32 n = 0;
   u64 start, elapsed;

   start = RDTSC();

   while (true) {

      added = enqueue_job(0, &test_tasklet_func, NULL);

      if (!added)
         break;

      n++;
   }

   elapsed = RDTSC() - start;

   ASSERT(n > 0); // SA: avoid division by zero warning
   printk("Avg. tasklet enqueue cycles: %llu [%i tasklets]\n", elapsed/n, n);
   regular_self_test_end();
}
