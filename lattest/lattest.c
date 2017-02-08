/**
 * Small kernel module to test the latency variance
 *
 * Heavily influenced by http://stackoverflow.com/questions/16920238/reliability-of-linux-kernel-add-timer-at-resolution-of-one-jiffy
 *
 * Johann Glaser
 */

#include <linux/module.h>	/* Needed by all modules */
#include <linux/kernel.h>	/* Needed for KERN_INFO */
#include <linux/init.h>         /* Needed for the macros */
#include <linux/jiffies.h>
#include <linux/time.h>
#include <linux/hrtimer.h>

#define MAXRUNS 2000

#define HIST_BIN_NUM  20  // number of histogram bins
#define HIST_BIN_LSBS 10  // 2^LSBS ns width of each bin

static volatile int runcount = 0;

static unsigned long period_ms;
static unsigned long period_ns;
static ktime_t ktime_period_ns;
static struct hrtimer my_hrtimer;
static long long last_kt_now;

// histogram
static unsigned int histogram[HIST_BIN_NUM];
static long long hist_min = LLONG_MAX;
static long long hist_max = LLONG_MIN;

static enum hrtimer_restart lattest_timer_function(struct hrtimer *timer) {
  unsigned long tjnow;
  ktime_t kt_now;
  long long kt_now_ns;
  long long diff_ns;
  int ret_overrun;
  long long hist_bin;

  runcount++;

  // printk(KERN_INFO " %s: runcount %d \n", __func__, runcount);

  tjnow       = jiffies;
  kt_now      = hrtimer_cb_get_time(&my_hrtimer);
  kt_now_ns   = ktime_to_ns(kt_now);
  diff_ns     = kt_now_ns - last_kt_now;
  last_kt_now = kt_now_ns;
  ret_overrun = hrtimer_forward(&my_hrtimer, kt_now, ktime_period_ns);
//  printk(KERN_INFO " lattest jiffies %lu ; ret: %d ; ktnsec: %lld = +%ldms %+lldns\n",
//    tjnow, ret_overrun, kt_now_ns, period_ms, diff_ns - period_ns);
  // histogram
  if (runcount > 1) {
    diff_ns = diff_ns - period_ns;   // reuse variable
    // e.g., 0..1023 --> 10, 1024..2047 --> 11
    hist_bin = (diff_ns >> HIST_BIN_LSBS) + (HIST_BIN_NUM >> 1);
    if (hist_bin < 0) hist_bin = 0;
    if (hist_bin > HIST_BIN_NUM-1) hist_bin = HIST_BIN_NUM-1;
    histogram[hist_bin]++;
    if (diff_ns < hist_min) hist_min = diff_ns;
    if (diff_ns > hist_max) hist_max = diff_ns;
  }

  if (runcount < MAXRUNS) {
    return HRTIMER_RESTART;
  }
  // finished, don't restart the timer
  return HRTIMER_NORESTART;
}

static int __init lattest_init(void) {

  struct timespec tp_hr_res;
  period_ms = 1000/HZ;
  // hrtimer_get_res(CLOCK_MONOTONIC, &tp_hr_res);
  tp_hr_res.tv_sec  = 0;
  tp_hr_res.tv_nsec = hrtimer_resolution;
  printk(KERN_INFO
    "Init lattest: %d ; HZ: %d ; 1/HZ (ms): %ld ; hrres: %lld.%.9ld\n",
               runcount,      HZ,        period_ms, (long long)tp_hr_res.tv_sec, tp_hr_res.tv_nsec );

  hrtimer_init(&my_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
  my_hrtimer.function = &lattest_timer_function;
  period_ns = period_ms*( (unsigned long)1E6L );

  printk(KERN_INFO " period_ms = %ldms, period_ns = %ldns\n", period_ms, period_ns);

  ktime_period_ns = ktime_set(0,period_ns);
  hrtimer_start(&my_hrtimer, ktime_period_ns, HRTIMER_MODE_REL);

  // A non 0 return means init_module failed; module can't be loaded. 
  return 0;
}

static void __exit lattest_exit(void) {
  int ret_cancel = 0;
  int i;
  while( hrtimer_callback_running(&my_hrtimer) ) {
    ret_cancel++;
  }
  if (ret_cancel != 0) {
    printk(KERN_INFO " lattest Waited for hrtimer callback to finish (%d)\n", ret_cancel);
  }
  if (hrtimer_active(&my_hrtimer) != 0) {
    ret_cancel = hrtimer_cancel(&my_hrtimer);
    printk(KERN_INFO " lattest active hrtimer cancelled: %d (%d)\n", ret_cancel, runcount);
  }
  if (hrtimer_is_queued(&my_hrtimer) != 0) {
    ret_cancel = hrtimer_cancel(&my_hrtimer);
    printk(KERN_INFO " lattest queued hrtimer cancelled: %d (%d)\n", ret_cancel, runcount);
  }

  // report histogram
  printk(KERN_INFO "Min: %+lldns\n", hist_min);
  printk(KERN_INFO " <  %+6dns: %d\n", (1-HIST_BIN_LSBS) << HIST_BIN_LSBS, histogram[0]);
  for (i = 1; i < HIST_BIN_NUM; i++) {
    printk(KERN_INFO " >= %+6dns: %d\n", (i-HIST_BIN_LSBS) << HIST_BIN_LSBS, histogram[i]);
  }
  printk(KERN_INFO "Max: %+lldns\n", hist_max);
  printk(KERN_INFO "Exit lattest\n");
}

module_init(lattest_init);
module_exit(lattest_exit);

MODULE_LICENSE("GPL");
