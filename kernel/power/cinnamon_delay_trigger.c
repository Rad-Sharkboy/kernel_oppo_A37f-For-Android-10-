/*
 * Cinnamon_Active - Execute commands after delay
 * 
 * This module provides a delayed execution mechanism for power management
 * commands. It automatically triggers Cinnamon_Active 30 seconds after boot
 * to allow system stabilization.
 *
 * Features:
 * - 30 second delayed execution on module load
 * - Manual control via proc interface
 * - zRAM, VM, and scheduler optimizations
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <dt-bindings/clock/msm-clocks-8916.h>

/* Delay period in milliseconds */
#define CINNAMON_DELAY_MS	30000	/* 30 seconds */

/* Work structure for delayed execution */
static struct delayed_work cinnamon_delay_work;
static struct delayed_work cinnamon_usb_reapply_work;
static struct delayed_work cinnamon_scheduler_reapply_work;
static struct delayed_work cinnamon_bimc_retry_work;
static struct workqueue_struct *cinnamon_wq;
static bool delay_pending = false;
static bool auto_trigger = true;  /* Auto-run when module loads */
static unsigned int usb_reapply_delay_ms = 30000;
static unsigned int usb_reapply_retries = 3;
static unsigned int scheduler_reapply_delay_ms = 120000;  /* 120s, after boot_completed */

module_param(auto_trigger, bool, 0644);
MODULE_PARM_DESC(auto_trigger, "Enable auto execution after boot");

module_param(usb_reapply_delay_ms, uint, 0644);
module_param(usb_reapply_retries, uint, 0644);

/* Proc entry */
static struct proc_dir_entry *cinnamon_delay_proc_entry;

static int cinnamon_file_exists(const char *path);
static int cinnamon_read_path(char *buf, size_t buflen, const char *path);

static int cinnamon_write_path(const char *path, const char *value)
{
	struct file *file;
	mm_segment_t old_fs;
	loff_t pos = 0;
	ssize_t ret;

	file = filp_open(path, O_WRONLY, 0);
	if (IS_ERR(file)) {
		pr_err("Cinnamon_Active: Failed to open %s: %ld\n", path, PTR_ERR(file));
		return (int)PTR_ERR(file);
	}

	old_fs = get_fs();
	set_fs(KERNEL_DS);
	ret = vfs_write(file, value, strlen(value), &pos);
	set_fs(old_fs);
	filp_close(file, NULL);

	if (ret < 0) {
		pr_err("Cinnamon_Active: Failed to write %s to %s: %zd\n", value, path, ret);
		return (int)ret;
	}

	pr_info("Cinnamon_Active: Wrote %s to %s\n", value, path);
	return 0;
}

static void cinnamon_apply_usb_gadget(void)
{
	char tmp[64];

	if (!cinnamon_file_exists("/sys/class/android_usb/android0/enable"))
		return;

	if (cinnamon_write_path("/sys/class/android_usb/android0/enable", "0") != 0)
		pr_err("Cinnamon_Active: Failed to disable gadget\n");

	msleep(500);

	if (cinnamon_write_path("/sys/class/android_usb/android0/functions", "ffs,hid") != 0)
		pr_err("Cinnamon_Active: Failed to set functions\n");

	if (cinnamon_read_path(tmp, sizeof(tmp), "/sys/class/android_usb/android0/functions") == 0)
		pr_info("Cinnamon_Active: functions now %s", tmp);

	msleep(300);

	if (cinnamon_write_path("/sys/class/android_usb/android0/enable", "1") != 0)
		pr_err("Cinnamon_Active: Failed to enable gadget\n");
}

static void cinnamon_usb_reapply_work_fn(struct work_struct *work)
{
	char tmp[64];
	unsigned int retries;

	retries = usb_reapply_retries;
	if (retries == 0)
		retries = 1;

	while (retries--) {
		if (cinnamon_read_path(tmp, sizeof(tmp), "/sys/class/android_usb/android0/functions") == 0) {
			if (!strncmp(tmp, "ffs,hid", 7))
				return;
		}
		cinnamon_apply_usb_gadget();
		msleep(500);
	}
}

static int cinnamon_read_path(char *buf, size_t buflen, const char *path)
{
	struct file *file;
	mm_segment_t old_fs;
	loff_t pos = 0;
	ssize_t ret;

	if (!buf || buflen == 0)
		return -EINVAL;

	file = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(file))
		return (int)PTR_ERR(file);

	old_fs = get_fs();
	set_fs(KERNEL_DS);
	ret = vfs_read(file, buf, buflen - 1, &pos);
	set_fs(old_fs);
	filp_close(file, NULL);

	if (ret < 0)
		return (int)ret;

	buf[ret] = '\0';
	return 0;
}

/* Helper function to check if file exists */
static int cinnamon_file_exists(const char *path)
{
	struct file *file;
	int exists = 0;
	
	file = filp_open(path, O_RDONLY, 0);
	if (!IS_ERR(file)) {
		exists = 1;
		filp_close(file, NULL);
	}
	return exists;
}

/* Dynamic block device detection */
static char *cinnamon_find_main_block(void)
{
	/* Check common block devices in order of preference */
	if (cinnamon_file_exists("/sys/block/mmcblk0/queue/scheduler")) {
		pr_info("Cinnamon_Active: Found main block device: mmcblk0\n");
		return "mmcblk0";
	}
	if (cinnamon_file_exists("/sys/block/sda/queue/scheduler")) {
		pr_info("Cinnamon_Active: Found main block device: sda\n");
		return "sda";
	}
	if (cinnamon_file_exists("/sys/block/nvme0n1/queue/scheduler")) {
		pr_info("Cinnamon_Active: Found main block device: nvme0n1\n");
		return "nvme0n1";
	}
	if (cinnamon_file_exists("/sys/block/ufs0/queue/scheduler")) {
		pr_info("Cinnamon_Active: Found main block device: ufs0\n");
		return "ufs0";
	}
	pr_warn("Cinnamon_Active: No suitable block device found\n");
	return NULL;
}

/* Cinnamon_Active command functions */
static void cinnamon_execute_command_1(void)
{
	pr_info("Cinnamon_Active: Executing Cinnamon_Active - Step 1\n");
	
	/* Step 1: Cleanup old zRAM and reset */
	{
		/* Check if zram0 exists before proceeding */
		if (!cinnamon_file_exists("/sys/block/zram0/comp_algorithm")) {
			pr_err("Cinnamon_Active: zram0 not available\n");
			return;
		}
		
		/* Minimal wait for system stabilization */
		msleep(50);
		
		/* echo 1 > /sys/block/zram0/reset */
		if (cinnamon_write_path("/sys/block/zram0/reset", "1") != 0) {
			pr_err("Cinnamon_Active: Failed to reset zram0\n");
		}
		
		/* Reduced wait - reset usually completes quickly */
		msleep(100);
	}
	pr_info("Cinnamon_Active: zRAM cleanup completed\n");
}

static void cinnamon_execute_command_2(void)
{
	pr_info("Cinnamon_Active: Executing Cinnamon_Active - Step 2\n");
	
	/* Step 2: Setup Cinnamon compression and disk size */
	{
		/* Check if zram0 exists before proceeding */
		if (!cinnamon_file_exists("/sys/block/zram0/comp_algorithm")) {
			pr_err("Cinnamon_Active: zram0 not available for setup\n");
			return;
		}
		
		/* Reduced wait - previous step already completed */
		msleep(100);
		
		/* echo cinnamon > /sys/block/zram0/comp_algorithm */
		if (cinnamon_write_path("/sys/block/zram0/comp_algorithm", "cinnamon") != 0) {
			pr_err("Cinnamon_Active: Failed to set comp_algorithm\n");
		}
		
		/* Minimal wait between sysfs operations */
		msleep(50);
		
		/* echo 805306368 > /sys/block/zram0/disksize */
		if (cinnamon_write_path("/sys/block/zram0/disksize", "805306368") != 0) {
			pr_err("Cinnamon_Active: Failed to set disksize\n");
		}

		msleep(200);
		{
			char *argv[] = {"/system/bin/mkswap", "/dev/block/zram0", NULL};
			char *envp[] = {"PATH=/system/bin:/system/xbin", NULL};
			int ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
			if (ret != 0)
				pr_err("Cinnamon_Active: mkswap failed with code %d\n", ret);
			else
				pr_info("Cinnamon_Active: mkswap completed successfully\n");
		}

		msleep(500);
		{
			char *argv[] = {"/system/bin/swapon", "/dev/block/zram0", "-p", "10", NULL};
			char *envp[] = {"PATH=/system/bin:/system/xbin", NULL};
			int ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
			if (ret != 0)
				pr_err("Cinnamon_Active: swapon failed with code %d\n", ret);
			else
				pr_info("Cinnamon_Active: swapon completed successfully\n");
		}
	}
	pr_info("Cinnamon_Active: zRAM compression and swap setup completed\n");
}

static int cinnamon_apply_block_tuning(void)
{
	char *main_block;
	char scheduler_path[256];
	char readahead_path[256];

	main_block = cinnamon_find_main_block();
	if (!main_block) {
		pr_err("Cinnamon_Active: Failed to find main block device\n");
		return -ENODEV;
	}

	snprintf(scheduler_path, sizeof(scheduler_path),
		"/sys/block/%s/queue/scheduler", main_block);
	snprintf(readahead_path, sizeof(readahead_path),
		"/sys/block/%s/queue/read_ahead_kb", main_block);

	if (cinnamon_write_path(scheduler_path, "cinnamon") != 0)
		pr_err("Cinnamon_Active: Failed to set scheduler\n");

	msleep(25);

	if (cinnamon_write_path(readahead_path, "512") != 0)
		pr_err("Cinnamon_Active: Failed to set read_ahead_kb\n");

	msleep(25);

	{
		char *argv[] = {"/system/bin/setprop", "sys.io.scheduler", "cinnamon", NULL};
		char *envp[] = {"PATH=/system/bin:/system/xbin", NULL};
		if (call_usermodehelper(argv[0], argv, envp, UMH_NO_WAIT) != 0)
			pr_err("Cinnamon_Active: Failed to set sys.io.scheduler property\n");
		else
			pr_info("Cinnamon_Active: Set sys.io.scheduler=cinnamon\n");
	}

	msleep(25);

	{
		char *argv[] = {"/system/bin/setprop", "persist.sys.io.scheduler", "cinnamon", NULL};
		char *envp[] = {"PATH=/system/bin:/system/xbin", NULL};
		call_usermodehelper(argv[0], argv, envp, UMH_NO_WAIT);
	}

	msleep(25);

	{
		char *argv[] = {"/system/bin/chmod", "644", "/dev/frandom", "/dev/erandom", NULL};
		char *envp[] = {"PATH=/system/bin:/system/xbin", NULL};
		call_usermodehelper(argv[0], argv, envp, UMH_NO_WAIT);
	}

	return 0;
}

static void cinnamon_scheduler_reapply_work_fn(struct work_struct *work)
{
	char scheduler_path[256];
	char *main_block;
	unsigned int retries = 5;

	main_block = cinnamon_find_main_block();
	if (!main_block) {
		pr_err("Cinnamon_Active: Scheduler reapply: failed to find main block\n");
		return;
	}

	snprintf(scheduler_path, sizeof(scheduler_path),
		"/sys/block/%s/queue/scheduler", main_block);

	while (retries--) {
		if (cinnamon_write_path(scheduler_path, "cinnamon") == 0) {
			pr_info("Cinnamon_Active: Scheduler re-applied cinnamon\n");
			goto set_prop;
		}
		msleep(100);
	}

	pr_err("Cinnamon_Active: Scheduler reapply failed after retries\n");
	return;

set_prop:
	{
		char *argv[] = {"/system/bin/setprop", "sys.io.scheduler", "cinnamon", NULL};
		char *envp[] = {"PATH=/system/bin:/system/xbin", NULL};
		call_usermodehelper(argv[0], argv, envp, UMH_NO_WAIT);
	}

	/* Reapply VM tunables after Android init overrode them */
	cinnamon_write_path("/proc/sys/vm/extra_free_kbytes", "16384");
	msleep(10);
	cinnamon_write_path("/proc/sys/vm/swappiness", "80");
	msleep(10);
	cinnamon_write_path("/proc/sys/vm/vfs_cache_pressure", "50");
	msleep(10);
	cinnamon_write_path("/proc/sys/net/ipv4/tcp_congestion_control", "westwood");

	/* Reapply interactive governor tunings after Android power HAL override */
	msleep(10);
	cinnamon_write_path("/sys/devices/system/cpu/cpufreq/interactive/go_hispeed_load", "75");
	msleep(10);
	cinnamon_write_path("/sys/devices/system/cpu/cpufreq/interactive/hispeed_freq", "998400");
	msleep(10);
	cinnamon_write_path("/sys/devices/system/cpu/cpufreq/interactive/min_sample_time", "30000");
	msleep(10);
	cinnamon_write_path("/sys/devices/system/cpu/cpufreq/interactive/timer_rate", "20000");
	msleep(10);
	cinnamon_write_path("/sys/devices/system/cpu/cpufreq/interactive/timer_slack", "20000");
	msleep(10);
	cinnamon_write_path("/sys/devices/system/cpu/cpufreq/interactive/above_hispeed_delay", "20000");
	msleep(10);
	cinnamon_write_path("/sys/devices/system/cpu/cpufreq/interactive/boostpulse_duration", "80000");
	msleep(10);
	cinnamon_write_path("/sys/devices/system/cpu/cpufreq/interactive/target_loads",
		"1 200000:25 400000:35 533333:45 800000:55 998400:60 1094400:65 1152000:70 1209600:80");
}

static void cinnamon_try_bimc_oc(void)
{
	struct clk *bimc_clk;
	struct device_node *np;
	struct of_phandle_args clkspec;
	long rate;

	np = of_find_compatible_node(NULL, NULL, "qcom,rpmcc-8916");
	if (!np) {
		pr_err("Cinnamon_Active: No rpmcc-8916 DT node found\n");
		return;
	}

	clkspec.np = np;
	clkspec.args_count = 1;
	clkspec.args[0] = clk_bimc_clk;
	bimc_clk = of_clk_get_from_provider(&clkspec);
	of_node_put(np);
	if (IS_ERR(bimc_clk)) {
		pr_info("Cinnamon_Active: bimc_clk not ready yet: %ld, will retry\n", PTR_ERR(bimc_clk));
		queue_delayed_work(cinnamon_wq, &cinnamon_bimc_retry_work,
			msecs_to_jiffies(30000));
		return;
	}

	rate = clk_get_rate(bimc_clk);
	pr_info("Cinnamon_Active: BIMC clock rate before: %ld Hz\n", rate);

	rate = clk_round_rate(bimc_clk, 600000000);
	pr_info("Cinnamon_Active: BIMC round_rate(600MHz) = %ld Hz\n", rate);

	clk_set_rate(bimc_clk, 600000000);
	msleep(10);

	rate = clk_get_rate(bimc_clk);
	pr_info("Cinnamon_Active: BIMC clock rate after: %ld Hz\n", rate);

	clk_put(bimc_clk);
}

static void cinnamon_bimc_retry_work_fn(struct work_struct *work)
{
	cinnamon_try_bimc_oc();
}

static void cinnamon_execute_command_3(void)
{
	pr_info("Cinnamon_Active: Executing Cinnamon_Active - Step 3\n");
	
	/* Step 3: VM optimization */
	{
		/* Minimal wait - previous step already completed */
		msleep(50);
		
		/* echo 80 > /proc/sys/vm/swappiness */
		if (cinnamon_write_path("/proc/sys/vm/swappiness", "80") != 0) {
			pr_err("Cinnamon_Active: Failed to set swappiness\n");
		}
		
		/* Minimal wait between VM operations */
		msleep(25);
		
		/* echo 50 > /proc/sys/vm/vfs_cache_pressure */
		if (cinnamon_write_path("/proc/sys/vm/vfs_cache_pressure", "50") != 0) {
			pr_err("Cinnamon_Active: Failed to set vfs_cache_pressure\n");
		}
		
		/* Minimal wait between VM operations */
		msleep(25);
		
		/* echo 5 > /proc/sys/vm/dirty_background_ratio */
		if (cinnamon_write_path("/proc/sys/vm/dirty_background_ratio", "5") != 0) {
			pr_err("Cinnamon_Active: Failed to set dirty_background_ratio\n");
		}
		
		/* Minimal wait between VM operations */
		msleep(25);
		
		/* echo 15 > /proc/sys/vm/dirty_ratio */
		if (cinnamon_write_path("/proc/sys/vm/dirty_ratio", "15") != 0) {
			pr_err("Cinnamon_Active: Failed to set dirty_ratio\n");
		}
		
		msleep(25);
		
		/* extra_free_kbytes = 16384: give kswapd more headroom, reduce direct reclaim stalls */
		if (cinnamon_write_path("/proc/sys/vm/extra_free_kbytes", "16384") != 0) {
			pr_err("Cinnamon_Active: Failed to set extra_free_kbytes\n");
		}
	}
	
	/* Step 4: Scheduler optimization */
	{
		msleep(50);
		cinnamon_apply_block_tuning();
	}

	/* Step 5: Interactive governor tuning */
	{
		int i;
		const char *cpu_policies[] = {
			"/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",
			"/sys/devices/system/cpu/cpu1/cpufreq/scaling_governor",
			"/sys/devices/system/cpu/cpu2/cpufreq/scaling_governor",
			"/sys/devices/system/cpu/cpu3/cpufreq/scaling_governor",
		};

		msleep(50);

		/* Switch from performance to interactive governor */
		for (i = 0; i < 4; i++) {
			if (cinnamon_file_exists(cpu_policies[i]))
				cinnamon_write_path(cpu_policies[i], "interactive");
		}

		msleep(50);

		/* Tune interactive governor for low-latency UI response */
		cinnamon_write_path("/sys/devices/system/cpu/cpufreq/interactive/go_hispeed_load", "75");
		msleep(10);
		cinnamon_write_path("/sys/devices/system/cpu/cpufreq/interactive/hispeed_freq", "998400");
		msleep(10);
		cinnamon_write_path("/sys/devices/system/cpu/cpufreq/interactive/min_sample_time", "40000");
		msleep(10);
		cinnamon_write_path("/sys/devices/system/cpu/cpufreq/interactive/timer_rate", "10000");
		msleep(10);
		cinnamon_write_path("/sys/devices/system/cpu/cpufreq/interactive/target_loads", "80");
		msleep(10);
		cinnamon_write_path("/sys/devices/system/cpu/cpufreq/interactive/above_hispeed_delay", "20000");
		msleep(10);
		cinnamon_write_path("/sys/devices/system/cpu/cpufreq/interactive/io_is_busy", "1");
		msleep(10);
		cinnamon_write_path("/sys/devices/system/cpu/cpufreq/interactive/boostpulse_duration", "40000");
	}

	/* Step 6: USB Gadget setup for ADB+HID */
	{
		msleep(100);
		cinnamon_apply_usb_gadget();
	}

	/* Step 7: LMK tuning for 2GB RAM */
	{
		msleep(50);
		if (cinnamon_file_exists("/sys/module/lowmemorykiller/parameters/minfree")) {
			cinnamon_write_path("/sys/module/lowmemorykiller/parameters/minfree",
				"7168,8192,11776,15360,18944,22528");
			msleep(10);
			cinnamon_write_path("/sys/module/lowmemorykiller/parameters/adj",
				"0,100,200,300,528,900");
		}
	}

	/* Step 8: TCP Westwood+ for lower wireless latency */
	{
		msleep(25);
		cinnamon_write_path("/proc/sys/net/ipv4/tcp_congestion_control", "westwood");
	}
	pr_info("Cinnamon_Active: VM, interactive governor, USB gadget, LMK, and TCP Westwood optimization completed\n");
}

/* Main work function - executes after delay */
static void cinnamon_delay_work_fn(struct work_struct *work)
{
	pr_info("Cinnamon_Active: Auto-executing Cinnamon_Active after %d ms\n", 
		CINNAMON_DELAY_MS);
	
	delay_pending = false;
	
	pr_info("Cinnamon_Active: Executing Step 1\n");
	cinnamon_execute_command_1();
	
	pr_info("Cinnamon_Active: Executing Step 2\n");
	cinnamon_execute_command_2();
	
	pr_info("Cinnamon_Active: Executing Step 3\n");
	cinnamon_execute_command_3();

	/* Step 4: Enable DT2W at boot */
	{
		msleep(50);
		if (cinnamon_file_exists("/proc/touchpanel/double_tap_enable"))
			cinnamon_write_path("/proc/touchpanel/double_tap_enable", "1");
	}

	/* Step 5: Try to force BIMC (DDR) clock to 600 MHz */
	cinnamon_try_bimc_oc();

	/* Step 6: Enable Adreno Idler */
	{
		const char *gpu_paths[] = {
			"/sys/devices/soc.0/1c00000.qcom,kgsl-3d0/kgsl/kgsl-3d0/gpu_idler",
			"/sys/devices/soc.0/fdb00000.qcom,kgsl-3d0/kgsl/kgsl-3d0/gpu_idler",
			NULL
		};
		int i;
		for (i = 0; gpu_paths[i]; i++) {
			if (cinnamon_file_exists(gpu_paths[i])) {
				cinnamon_write_path(gpu_paths[i], "1");
				break;
			}
		}
	}

	/* Step 7: Block known battery-draining wakelocks */
	{
		const char *block_list[] = {
			"+wlan_wake", "+wlan_rx_wake", "+wlan_ctrl_wake",
			"+wlan_ipa", "+wlan_pno_wake", "+netmgr_wake",
			"+IPA_WS", "+qcom_rx_wakelock", "+ss_route_work",
			"+radio-interface", "+qcril", "+ims_socket_wake",
			"+CNE_WAKELOCK", "+cne_wqe_wake", "+power_policy_wake",
			NULL
		};
		int i;
		for (i = 0; block_list[i]; i++) {
			if (cinnamon_file_exists("/proc/wakelock_blocker"))
				cinnamon_write_path("/proc/wakelock_blocker",
					block_list[i]);
		}
	}

	queue_delayed_work(cinnamon_wq, &cinnamon_usb_reapply_work,
		msecs_to_jiffies(usb_reapply_delay_ms));

	queue_delayed_work(cinnamon_wq, &cinnamon_scheduler_reapply_work,
		msecs_to_jiffies(scheduler_reapply_delay_ms));

	
	pr_info("Cinnamon_Active: Auto execution completed\n");
}

/* Start delayed execution */
static void cinnamon_start_delay(void)
{
	if (delay_pending) {
		pr_info("Cinnamon_Active: Delay already in progress, cancelling previous\n");
		cancel_delayed_work_sync(&cinnamon_delay_work);
		delay_pending = false;
	}
	
	pr_info("Cinnamon_Active: Starting %d second delay\n", 
		CINNAMON_DELAY_MS / 1000);
 	
	if (queue_delayed_work(cinnamon_wq, &cinnamon_delay_work,
		msecs_to_jiffies(CINNAMON_DELAY_MS))) {
		delay_pending = true;
	} else {
		pr_err("schedule_delayed_work failed\n");
	}
}

/* Cancel delayed execution */
static void cinnamon_cancel_delay(void)
{
	if (delay_pending) {
		pr_info("Cinnamon_Active: Cancelling delayed execution\n");
		cancel_delayed_work_sync(&cinnamon_delay_work);
		delay_pending = false;
	} else {
		pr_info("Cinnamon_Active: No delay in progress\n");
	}
}

/* Proc interface - show status */
static int cinnamon_delay_show(struct seq_file *m, void *v)
{
	seq_printf(m, "Cinnamon_Active Status:\n");
	seq_printf(m, "Delay Period: %d seconds\n", CINNAMON_DELAY_MS / 1000);
	seq_printf(m, "Delay Pending: %s\n", delay_pending ? "YES" : "NO");
	seq_printf(m, "Workqueue Status: %s\n", delay_pending ? "SCHEDULED" : "IDLE");
	seq_printf(m, "Auto Trigger on Load: %s\n", auto_trigger ? "ENABLED" : "DISABLED");
	seq_printf(m, "\nCinnamon_Active Commands:\n");
	seq_printf(m, "1 - Start Cinnamon_Active (%d second delay)\n", CINNAMON_DELAY_MS / 1000);
	seq_printf(m, "2 - Cancel Cinnamon_Active\n");
	seq_printf(m, "3 - Force immediate Cinnamon_Active\n");
	seq_printf(m, "4 - Disable auto trigger on next boot (temporary)\n");
	seq_printf(m, "5 - Enable auto trigger\n");
	seq_printf(m, "6 - Re-apply block tuning (detect + scheduler/read_ahead)\n");
	seq_printf(m, "7 - Re-apply USB gadget (ffs,hid)\n");
	return 0;
}

/* Proc interface - write control */
static ssize_t cinnamon_delay_write(struct file *file, const char __user *buf,
				    size_t count, loff_t *ppos)
{
	char kbuf[16];
	int val;
	
	if (count > sizeof(kbuf) - 1)
		return -EINVAL;
		
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
		
	kbuf[count] = '\0';
	if (sscanf(kbuf, "%d", &val) != 1)
		return -EINVAL;
		
	switch (val) {
	case 1:
		pr_info("Cinnamon_Active: Manual start with delay\n");
		cinnamon_start_delay();
		break;
	case 2:
		pr_info("Cinnamon_Active: Manual cancel\n");
		cinnamon_cancel_delay();
		break;
	case 3:
		pr_info("Cinnamon_Active: Force immediate execution\n");
		if (delay_pending) {
			cancel_delayed_work_sync(&cinnamon_delay_work);
			delay_pending = false;
		}
		if (!queue_work(cinnamon_wq, &cinnamon_delay_work.work))
			pr_info("Cinnamon_Active: Work already queued, skipping force schedule\n");
		break;
	case 4:
		pr_info("Cinnamon_Active: Disable auto trigger for this boot\n");
		auto_trigger = false;
		break;
	case 5:
		pr_info("Cinnamon_Active: Enable auto trigger\n");
		auto_trigger = true;
		break;
	case 6:
		pr_info("Cinnamon_Active: Re-applying block tuning\n");
		cinnamon_apply_block_tuning();
		break;
	case 7:
		pr_info("Cinnamon_Active: Re-applying USB gadget\n");
		cinnamon_apply_usb_gadget();
		break;
	default:
		return -EINVAL;
	}
	
	return count;
}

/* Proc file operations */
static int cinnamon_delay_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, cinnamon_delay_show, NULL);
}

static const struct file_operations cinnamon_delay_fops = {
	.owner		= THIS_MODULE,
	.open		= cinnamon_delay_proc_open,
	.read		= seq_read,
	.write		= cinnamon_delay_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* Module initialization */
static int __init cinnamon_delay_trigger_init(void)
{
	pr_info("Cinnamon_Active: Module init STARTING\n");
	
	cinnamon_wq = alloc_workqueue("cinnamon_wq", WQ_UNBOUND | WQ_HIGHPRI, 1);
	if (!cinnamon_wq) {
		pr_err("Cinnamon_Active: Failed to alloc workqueue\n");
		return -ENOMEM;
	}
  	
  	INIT_DELAYED_WORK(&cinnamon_delay_work, cinnamon_delay_work_fn);
	INIT_DELAYED_WORK(&cinnamon_usb_reapply_work, cinnamon_usb_reapply_work_fn);
	INIT_DELAYED_WORK(&cinnamon_scheduler_reapply_work, cinnamon_scheduler_reapply_work_fn);
	INIT_DELAYED_WORK(&cinnamon_bimc_retry_work, cinnamon_bimc_retry_work_fn);
	
	cinnamon_delay_proc_entry = proc_create_data("cinnamon_delay_trigger", 0666, NULL,
						&cinnamon_delay_fops, NULL);
	if (!cinnamon_delay_proc_entry) {
		pr_err("Cinnamon_Active: Failed to create proc entry\n");
		return -ENOMEM;
	}
	
	pr_info("Cinnamon_Active: auto_trigger parameter = %d\n", auto_trigger);
  	
  	/* Tự động schedule work sau 30 giây nếu auto_trigger được bật */
  	if (auto_trigger) {
  		pr_info("Cinnamon_Active: Auto-trigger enabled, scheduling in %d seconds\n",
  			CINNAMON_DELAY_MS / 1000);
		msleep(2000);
		if (queue_delayed_work(cinnamon_wq, &cinnamon_delay_work,
			msecs_to_jiffies(CINNAMON_DELAY_MS))) {
  			delay_pending = true;
  		} else {
  			pr_err("schedule_delayed_work failed\n");
  		}
  	} else {
  		pr_info("Cinnamon_Active: Auto-trigger disabled, waiting for manual command\n");
  	}
	
	pr_info("Cinnamon_Active: Module ready\n");
	return 0;
}

/* Module cleanup */
static void __exit cinnamon_delay_trigger_exit(void)
{
	pr_info("Cinnamon_Active: Shutting down\n");
	
	if (delay_pending) {
		cancel_delayed_work_sync(&cinnamon_delay_work);
		delay_pending = false;
	}

	cancel_delayed_work_sync(&cinnamon_usb_reapply_work);
	cancel_delayed_work_sync(&cinnamon_scheduler_reapply_work);
	cancel_delayed_work_sync(&cinnamon_bimc_retry_work);

	if (cinnamon_wq) {
		destroy_workqueue(cinnamon_wq);
		cinnamon_wq = NULL;
	}
  	
  	if (cinnamon_delay_proc_entry)
  		proc_remove(cinnamon_delay_proc_entry);
}

module_init(cinnamon_delay_trigger_init);
module_exit(cinnamon_delay_trigger_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Noveris");
MODULE_DESCRIPTION("Cinnamon_Active with auto-trigger");
MODULE_VERSION("2.2");