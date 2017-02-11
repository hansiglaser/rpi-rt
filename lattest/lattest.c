/**
 * Small kernel module to test the latency variance
 *
 * Heavily influenced by http://stackoverflow.com/questions/16920238/reliability-of-linux-kernel-add-timer-at-resolution-of-one-jiffy
 *
 * Features:
 *  - periodically executed function using hrtimer
 *  - statistics for variation of the latency
 *  - toggling GPIO
 *  - sysfs interface
 *
 * Control interface via SysFS
 *  - query current status: inactive/running, period, resolution, ...
 *  - set/get period
 *  - set number of periods or infinite
 *  - start / stop
 *  - query statistics
 *  - configure statistics: number and width of histogram bins
 *
 * /sys/class/LatTest/LatTest/
 *   .../status ...... (r) query current status: inactive/running, period, resolution, ...
 *   .../period ...... (rw) set/get period in ms
 *   .../control ..... (w) start (with number of periods) / stop
 *   .../config ...... (rw) configure statistics: number and width of histogram bins
 *   .../statistics .. (r) statistics: min, max, mean, stddev, histogram
 * The use of beautified output or of parsed input is strongly discouraged
 * according to https://www.kernel.org/doc/Documentation/filesystems/sysfs.txt.
 * However, we do it. :-)
 *
 * Author: Johann Glaser
 *
 * History
 *  2017-02-08 Created: hrtimer, histogram
 *  2017-02-11 Added GPIO, SysFS
 */

#include <linux/module.h>	/* Needed by all modules */
#include <linux/kernel.h>	/* Needed for KERN_INFO */
#include <linux/init.h>         /* Needed for the macros */
#include <linux/jiffies.h>
#include <linux/time.h>
#include <linux/hrtimer.h>
#include <linux/gpio.h>
#include <linux/device.h>
#include <linux/err.h>

//////////////////////////////////////////////////////////////////////////////
// Configuration /////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////

// define GPIO number, can be left undefined to disable the use of a GPIO
// GPIO 4 = physical pin 7, see e.g., http://pinout.xyz/pinout/pin7_gpio4
#define GPIO_LATTEST_TOGGLE 4

#define HIST_BIN_NUM  20  // number of histogram bins
#define HIST_BIN_LSBS 10  // 2^LSBS ns width of each bin

//////////////////////////////////////////////////////////////////////////////
// Variables /////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////

#ifdef GPIO_LATTEST_TOGGLE
static volatile int gpio_lattest_value;   // same type as used for gpio_set_value()
#endif // GPIO_LATTEST_TOGGLE

static unsigned long period_ms;   // period of the hrtimer
static struct hrtimer my_hrtimer; // hrtimer information structure
static volatile int runcount;     // number of timer occurences to run, is set >0 and decremented, -1 denotes infinite runs
static long long last_now_ns;     // last value of "now" in ns, is set to 0 to denote the first timer run

// statistics
// TODO: mean value, stddev
// histogram
static unsigned int histogram[HIST_BIN_NUM];
static long long hist_min = LLONG_MAX;
static long long hist_max = LLONG_MIN;

//////////////////////////////////////////////////////////////////////////////
// Timer Function ////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////

static enum hrtimer_restart lattest_timer_function(struct hrtimer *timer) {
  unsigned long tjnow;
  ktime_t now_kt;
  long long now_ns;
  ktime_t period_kt;
  long long diff_ns;
  int ret_overrun;
  long long hist_bin;

#ifdef GPIO_LATTEST_TOGGLE
  gpio_set_value(GPIO_LATTEST_TOGGLE, gpio_lattest_value);
  gpio_lattest_value = !gpio_lattest_value;
#endif // GPIO_LATTEST_TOGGLE

  tjnow       = jiffies;
  now_kt      = hrtimer_cb_get_time(&my_hrtimer);
  now_ns      = ktime_to_ns(now_kt);
  last_now_ns = now_ns;
  period_kt   = ktime_set(0, period_ms*1000000);
  ret_overrun = hrtimer_forward(&my_hrtimer, now_kt, period_kt);
  if (last_now_ns != 0) {
    diff_ns     = now_ns - last_now_ns;
//    printk(KERN_INFO " lattest jiffies %lu ; ret: %d ; ktnsec: %lld = +%ldms %+lldns\n",
//      tjnow, ret_overrun, now_kt_ns, period_ms, diff_ns - period_ns);
    // statistics
    diff_ns = diff_ns - period_ms*1000000;   // reuse variable
    // e.g., 0..1023 --> 10, 1024..2047 --> 11
    hist_bin = (diff_ns >> HIST_BIN_LSBS) + (HIST_BIN_NUM >> 1);
    if (hist_bin < 0) hist_bin = 0;
    if (hist_bin > HIST_BIN_NUM-1) hist_bin = HIST_BIN_NUM-1;
    histogram[hist_bin]++;
    if (diff_ns < hist_min) hist_min = diff_ns;
    if (diff_ns > hist_max) hist_max = diff_ns;
  }

  if (runcount > 0) {
    // decrement counter
    runcount--;
    return HRTIMER_RESTART;
  } else if (runcount == 0) {
    // finished, don't restart the timer
    return HRTIMER_NORESTART;
  } else /* if (runcount < 0) */ {
    // run infinitely
    return HRTIMER_RESTART;
  }
}

//////////////////////////////////////////////////////////////////////////////
// SysFS /////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////

/*
 *   .../status ...... (r) query current status: inactive/running, period, resolution, ...
 *   .../period ...... (rw) set/get period in ms
 *   .../control ..... (w) start (with number of periods) / stop
 *   .../config ...... (rw) configure statistics: number and width of histogram bins
 *   .../statistics .. (r) statistics: min, max, mean, stddev, histogram
 */

/**
 * Query current status: inactive/running, period, resolution, ...
 */
static ssize_t show_status_cb(struct device *dev, struct device_attribute *attr, char *buf) {
  return scnprintf(buf, PAGE_SIZE, "HZ: %d\nJiffie Period: %d ms\nHR timer resolution: %d ns\nLatTest period: %ld ms\nRunCount: %d\nStatus: %s\n",
    HZ, 1000/HZ, hrtimer_resolution, period_ms, runcount, ((runcount != 0) ? "running" : "stopped"));
}

/**
 * Query period
 */
static ssize_t show_period_cb(struct device *dev, struct device_attribute *attr, char *buf) {
  return scnprintf(buf, PAGE_SIZE, "%ld\n", period_ms);
}

/**
 * Set period in ms
 *
 * Max. 1 second allowed
 */
static ssize_t store_period_cb(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
  unsigned int new_period;
  if (runcount != 0) return -EINVAL;   // timer already running

  if (kstrtouint(buf, 10, &new_period) < 0) return -EINVAL;
  // max. 1 second allowed
  if (new_period> 1000) return -EINVAL;

  period_ms = new_period;
  printk(KERN_INFO "lattest: Setting period to %ld ms", period_ms);
  return count;
}

/**
 * Start (with number of periods or infinite) / stop
 *
 * "infinite"
 * nnn
 * "stop"
 */
static ssize_t store_control_cb(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
  int new_runcount;
  ktime_t period_kt;

  if (strncmp(buf, "stop", min((size_t)4, count)) == 0) {
    // stop the timer
    printk(KERN_INFO "lattest: Stopping the timer.");
    runcount = 0;
    return count;
  } else if (strncmp(buf, "infinite", min((size_t)8, count)) == 0) {
    if (runcount != 0) return -EINVAL;   // timer already running
    // run infinitely
    runcount = -1;
    printk(KERN_INFO "lattest: Starting the timer to run infinite times.");
  } else {
    if (runcount != 0) return -EINVAL;   // timer already running
    // run a given number of times
    if (kstrtoint(buf, 10, &new_runcount) < 0) return -EINVAL;
    if (new_runcount <= 0) return -EINVAL;
    // actually, kstrto*() only changes the result on success, but here the
    // compiler complains because the 'volatile' qualifier is discareded,
    // therefore we use an intermediate variable
    runcount = new_runcount;
    printk(KERN_INFO "lattest: Starting the timer to run %d times.", runcount);
  }
  // prepare for timer
  last_now_ns = 0;   // to denote the first run
  // reset statistics
  // TODO

  // start timer
  period_kt = ktime_set(0, period_ms*1000000);
  hrtimer_start(&my_hrtimer, period_kt, HRTIMER_MODE_REL);

  return count;
}

/**
 * Query statistics configuration
 */
static ssize_t show_config_cb(struct device *dev, struct device_attribute *attr, char *buf) {
  return 0;
}

/**
 * Configure statistics: number and width of histogram bins
 */
static ssize_t store_config_cb(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
  if (runcount != 0) return -EINVAL;   // timer already running

  return 0;
}

/**
 * Query statistics: min, max, mean, stddev, histogram
 */
static ssize_t show_statistics_cb(struct device *dev, struct device_attribute *attr, char *buf) {
  return 0;
}

static struct class  *s_pDeviceClass;
static struct device *s_pDeviceObject;
static DEVICE_ATTR(status,     S_IRUSR           | S_IRGRP           | S_IROTH          , show_status_cb,     NULL);
static DEVICE_ATTR(period,     S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH          , show_period_cb,     store_period_cb);
static DEVICE_ATTR(control,              S_IWUSR           | S_IWGRP                    , NULL,               store_control_cb);
static DEVICE_ATTR(config,     S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH          , show_config_cb,     store_config_cb);
static DEVICE_ATTR(statistics, S_IRUSR           | S_IRGRP           | S_IROTH          , show_statistics_cb, NULL);

//////////////////////////////////////////////////////////////////////////////
// Initialization & Finalization /////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////

static int __init lattest_init(void) {

  int ret;

  printk(KERN_INFO "Initializing LatTest: Small kernel module to test the latency variance\n");

#ifdef GPIO_LATTEST_TOGGLE
  // GPIO
  ret = gpio_request_one(GPIO_LATTEST_TOGGLE, GPIOF_OUT_INIT_LOW, "lattest");
  if (ret) {
    printk(KERN_ERR "Unable to request GPIOs: %d\n", ret);
    return ret;
  }
  gpio_lattest_value = 1;   // start with 0, ISR will set it to 1 at first call
#endif // GPIO_LATTEST_TOGGLE

  // SysFS
  s_pDeviceClass = class_create(THIS_MODULE, "LatTest");
  BUG_ON(IS_ERR(s_pDeviceClass));
  s_pDeviceObject = device_create(s_pDeviceClass, NULL, 0, NULL, "LatTest");
  BUG_ON(IS_ERR(s_pDeviceObject));
  ret = device_create_file(s_pDeviceObject, &dev_attr_status);
  BUG_ON(ret < 0);
  ret = device_create_file(s_pDeviceObject, &dev_attr_period);
  BUG_ON(ret < 0);
  ret = device_create_file(s_pDeviceObject, &dev_attr_control);
  BUG_ON(ret < 0);
  ret = device_create_file(s_pDeviceObject, &dev_attr_config);
  BUG_ON(ret < 0);
  ret = device_create_file(s_pDeviceObject, &dev_attr_statistics);
  BUG_ON(ret < 0);
  printk(KERN_INFO "  Registered sysfs attributes at /sys/class/LatTest/LatTest/\n");

  // set defaults
  runcount = 0;
  period_ms = 10;
  // TODO

  // timer
  hrtimer_init(&my_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
  my_hrtimer.function = &lattest_timer_function;

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

#ifdef GPIO_LATTEST_TOGGLE
  // GPIO
  gpio_set_value(GPIO_LATTEST_TOGGLE, 0);
  gpio_free(GPIO_LATTEST_TOGGLE);
#endif // GPIO_LATTEST_TOGGLE

  device_remove_file(s_pDeviceObject, &dev_attr_status);
  device_remove_file(s_pDeviceObject, &dev_attr_period);
  device_remove_file(s_pDeviceObject, &dev_attr_control);
  device_remove_file(s_pDeviceObject, &dev_attr_config);
  device_remove_file(s_pDeviceObject, &dev_attr_statistics);
  device_destroy(s_pDeviceClass, 0);
  class_destroy(s_pDeviceClass);

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
MODULE_AUTHOR("Johann Glaser");
MODULE_DESCRIPTION("Small kernel module to test the latency variance");

