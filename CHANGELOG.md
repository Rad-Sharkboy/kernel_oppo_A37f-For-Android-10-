# Cinnamon Kernel Changelog

## Development

### Leaner MMC Request Path (2026-10-27)

- **Removed:** optional MMC profiling counters, their request-path flag checks and per-host storage.
- **Preserved:** eMMC/SD transfer modes, clocks, normal I/O statistics and storage clock scaling.

### Leaner ARM64 Debug Metadata (2026-10-27)

- **Removed:** dynamic debug callsite tables and runtime flag checks from the A37f build.
- **Preserved:** regular printk/error logging, debugfs, tracing and Qualcomm crash-log symbols.

### Bounded zRAM Run History (2026-10-27)

- **Reduced compression work:** update at most four history slots for zero/repeated-byte runs, then advance the ring index directly.
- **Preserved:** compressed bytes and final history state in generic and ARM64 NEON paths.

### Restore Filesystem Sync Semantics (2026-10-27)

- **Fixed:** always complete the filesystem's requested fsync/fdatasync operation and return its actual result.
- **Removed:** battery-dependent fsync bypass, its proc toggle and recurring 60-second polling worker.

### Cinnamon zRAM History Search (2026-10-26)

- **Reduced compression work:** skip the repeated comparison against the newest history block after the quick-match check has already rejected it, in both ARM64 NEON and generic paths.
- **Preserved:** compressed format, history ordering, first-block handling and decompression behavior.

### Cinnamon I/O Merge Compatibility (2026-10-26)

- **Fixed:** validate request compatibility before front/back merges, including read/write direction, discard flags, device and encryption compatibility.
- **Preserved:** eight-request scan limit, queue ordering and write durability.

### Interactive Initial Boost Target (2026-10-26)

- **Changed:** `hispeed_freq` from 1209600 to 998400 kHz in both Cinnamon_Active startup and delayed reapply paths. Maximum CPU frequency remains 1209600 kHz.
- **Purpose:** reduce initial boost overhead during short CPU bursts, with potential battery savings during light use.
- **Preserved:** other governor settings, CPU/GPU overclocks, 65 Hz display, thermal policy and radio configuration.

### Cinnamon I/O Latency Accounting Fix (2026-10-25)

- **Fixed double averaging:** `total_latency_ns` already contains a moving average. Removed the extra division by request count from adaptive tuning and `avg_latency_ns` reporting; a 100 ms average no longer appears as 1 ms at count 100.
- **Preserved:** existing sampling, smoothing, manual-tuning grace period, CPU/GPU clocks, 65 Hz display setting and fsync behavior.
- **Validation:** local arithmetic regression passed; ARM64 GCC 4.9 compilation of `block/cinnamon-iosched.o` passed. Test harnesses remain local, outside the repository.
- **Pending:** full-image and device validation. Compare app responsiveness during file copies, throughput and copied-file hashes before claiming a speed or battery improvement.

## Builds #389–#402

### BQ24196 Bypass Charging + Real Current + Voltage SOC Fix (2026-07-01)

- **Build #389–#392:** `POWER_SUPPLY_PROP_BYPASS` sysfs (`/sys/class/power_supply/battery/bypass`). Safe bypass: disable charging only via `opchg_config_charging_disable(BYPASS_DISABLE)` — BATFET stays ON so system runs from USB via NVDC, battery supplements when load > input limit, seamless takeover on USB disconnect
- **Build #393:** Voltage-based SOC fallback when VM-BMS returns 0%: `(vol_uV - 3300000) / 10000`, cap 90%
- **Build #394:** Bypass uses OPPO multi-reason charging disable framework with `BYPASS_DISABLE = BIT(9)`. Verified REG01 bit 4 toggles (0x1b→0x0b), REG08 bit 5 clears (FAST_CHARGING→NO_CHARGING)
- **Build #395–#396:** Replaced hardcoded `CURRENT_NOW = -450` with real estimation from BQ24196 REG02 charge current limit: `((reg02 >> 2) + 1) * 64 mA`, capped by `max_input_current[INPUT_CURRENT_MIN] - 150 mA`. Reports ~350 mA on PC USB, ~1050 mA on 1.5A wall charger
- **Build #398–#400:** VM-BMS always returns `CAPACITY = 0` — override with voltage estimate divisor `/9000` (4.2V → 100%, cap 99%). Charging sync: increment SOC every 35s when voltage > 4.1V. Discharging: only decrement when voltage < 3.3V (prevented false drops but caused SOC freeze)
- **Build #401:** Auto-bypass — when `capacity ≥ 90%` AND charger present, auto-enable bypass; disable bypass when `capacity < 85%` OR charger removed. Hysteresis 5% avoids rapid toggling
- **Build #402:** Fixed SOC freeze during discharge — BMS always returns 0, so `soc_bms < bat_volt_check_point` condition was never true. Added voltage-based SOC comparison fallback: calculate `volt_soc = (vol_uV - 3300000) / 9000` and decrement when `volt_soc < bat_volt_check_point`. SOC now drops gradually based on voltage during stress test

## Builds #387–#388

### Monitor TX Injection Test — Redirect mon0 → wlan0 hdd_hard_start_xmit (2026-06-30)

- **Build #387:** Modified `hdd_mon_hard_start_xmit` (`wlan_hdd_tx_rx.c:501`) to redirect packets through wlan0's STA TX path instead of dropping them
- **Build #388:** Added `printk` debug to confirm the redirect path is hit
- **Test results:** No WMI raw/injection commands exist in the driver (`CORE/WMI/` directory doesn't exist, no `wlan_hal_msg.h` has raw TX message types beyond `WLAN_HAL_ENABLE_MONITOR_MODE_REQ/RSP` 302-303)
- `hdd_mon_hard_start_xmit` redirect path confirmed working via dmesg: `redirecting to wlan0` logged
- wlan0 TX counters incremented by exactly 5×60=300 bytes for 5 test packets sent through mon0 via AF_PACKET socket
- No firmware crash observed — **Kịch bản B confirmed**: firmware silently accepts redirected packets through STA data path
- **However:** Not true raw injection — packets go through normal STA TX path, encrypted with PTK, source MAC overwritten by firmware. Deauth/disassoc injection not possible
- **Conclusion:** Firmware can send arbitrary data payloads as encrypted STA data frames, but cannot inject raw 802.11 management frames. For WPA handshake capture, need alternative approach (EAPOL sniffer in wlan0 RX path)

## Builds #385–#386

### Monitor Mode Stability Fixes — TX Watchdog, Notifier, WCNSS Crash (2026-06-30)

- **Build #386:** Fixed notifier skip for mon\* interfaces — added separate early-return instead of nesting inside wlan/p2p condition
- **Fixed NETDEV WATCHDOG:** `netif_tx_start_all_queues()` instead of `netif_start_queue()` — `NUM_TX_QUEUES = 5` (WMM AC queues 0-4), single queue start leaves queues 1-4 unserviced → watchdog fires
- **Build #385:** Fixed VOS_ASSERT in netdev notifier — early return for `mon*` interfaces. VOS_ASSERT is `WARN_ON(1)` (cosmetic only, doesn't block anything)
- **Build #384:** Fixed WCNSS firmware crash on `iw dev mon0 del` — removed `WDA_MON_STOP_REQ` from `wlan_hdd_stop_mon()`. Stops monitor mode locally (set `state = MON_MODE_STOP`, re-enable BMPS/IMPS) without sending HAL command to firmware
- **Verified:** multiple create/delete cycles work reliably without crash

## Build #383

### Monitor Mode RX Path Fix — STA Coexistence via skb_copy (2026-06-30)

- **Fixed STA+MON co-existence:** Instead of `vos_pkt_set_os_packet()` (which rejects RX packet types), use `vos_pkt_get_os_packet(pVosPacket, &skb, VOS_FALSE)` to peek at skb without clearing it, then `skb_copy` to create independent copy for mon0
- STA path gets original skb untouched (0% packet loss verified), mon0 gets the copy with radiotap header
- Fixed typo `VOS_STATUS_E_SUCCESS` → `VOS_STATUS_SUCCESS`
- **Verified:** STA 0% ping loss while mon0 captures 50+ frames with radiotap

## Builds #379–#382

### WLAN Monitor Mode — Driver-Side Implementation (2026-06-30)

- **Standalone monitor mode:** Removed STA requirement in `wlan_hdd_add_monitor_check()` — now allows 0 or 1 STA interfaces (Build #378)
- **Forced FW capability bit:** `setFeatCaps(gpFwWlanFeatCaps, STA_MONITOR_SCC)` in `WDI_ProcessFeatureCapsExchangeRsp()` (Build #378)
- **Build #379:** `wlan_hdd_cfg80211_set_monitor_channel()` callback registered in cfg80211 ops, `WDA_MON_START_REQ` posted to firmware, waits 5s for response. `set freq` succeeds (`-EOPNOTSUPP` was for kernel ≥ 3.6)
- **Build #380:** Added monitor channel callback; `cfg80211_has_monitors_only` check removed from `cfg80211_set_monitor_channel()`. `set freq` succeeds but `WDA_MON_START_REQ` times out
- **Build #381:** Fixed `wlan_hdd_check_monitor_state()` + TL RX path for STA+MON (don't `continue` after monitor callback). Firmware still crashed — `wcnss` fatal error when `WDA_MON_START_REQ` sent
- **Build #382:** Removed `WDA_MON_START_REQ` — sets `state = MON_MODE_START` locally. Captured 10 frames with radiotap. STA data loss because `vos_pkt_set_os_packet()` rejects RX packet types
- **Key discovery:** WCNSS PIL blob (Cortex-R4/R5) doesn't implement `WLAN_HAL_ENABLE_MONITOR_MODE_REQ` (msgType 302) — sending it causes firmware crash/reboot. **All firmware HAL commands abandoned** in favor of local-only monitor mode
- **RX path architecture:** `WLANTL_RxFrames()` → calls both `WLANTL_HandleMonModeDataFrame()` and STA delivery. Monitor callback at `hdd_rx_packet_monitor_cbk()` delivers 802.11 + radiotap frame to mon0 via `netif_rx()`
- **hdd_mon_hard_start_xmit:** drops all TX (`kfree_skb(skb)` + `NETDEV_TX_OK`) — no injection support without firmware changes
- **Usage:** `iw phy phy0 interface add mon0 type monitor && ifconfig mon0 up && iw dev mon0 set freq 2412 && tcpdump -i mon0`
- **`airodump-ng mon0` works** in Debian chroot (SSH root@192.168.0.101) — captures AP "Nguyen T3" (WPA2, CH 10, -54 dBm) with station A8:1B:5A:19:00:AB, 6741 frames seen

## Build #378

### WLAN Monitor Mode — Force-enable (2026-06-30)

- **Pronto WCNSS driver:** Removed FW capability check (`sme_IsFeatureSupportedByFW(STA_MONITOR_SCC)`) in `wlan_hdd_add_monitor_check()` to allow `iw` to create a monitor interface
- Previously driver returned `-EINVAL` because WCNSS firmware doesn't report `STA_MONITOR_SCC` support
- May not capture frames if firmware doesn't implement monitor data path — test required

## Build #376 — Reverted

### eMMC HS400 + SD Card DDR50 — Failed (2026-06-30)

**Attempt:** eMMC HS400/200 MHz + SD card DDR50 via DTS `bus-speed-mode` and `clk-rates`.

**Result:** Bootloop — MSM8916 SDHCI controller can't handle HS400 or 200 MHz on this board.

**Revert:** commit `9e1a3dcfffe`.

## Build #372–#374 — Reverted

### PLL config_ctl_val Experiment — Failed (2026-06-29)

**Attempt:** CPU 1363 MHz via PLL CONFIG_CTL register (offset 0x14) with `config_ctl_val = 0x000D6968` (Snapdragon 810 SR2 PLL reference).

**Problem found:** `__variable_rate_pll_init` never called for SR2 PLL type — config_ctl_val not written to hardware. Fixed by adding init to `local_pll_clk_set_rate`.

**Result:**
- Config_ctl_val now takes effect (confirmed by performance change)
- 1363 MHz: **1106 ev/s** (half of 1209 MHz baseline) — wrong VCO settings corrupt PLL
- 1209 MHz: -6% regression (2144 ev/s) from init's USER_CTL side effects
- Writing 0 to CONFIG_CTL → **bootloop** (PLL needs valid analog config)

**Conclusion:** SR2 PLL VCO on 28nm LP is physically capped at ~1.2 GHz. No software PLL tuning can extend this.

**Revert:** All changes reverted in commit `e453afbfc4d`.

### Interactive Governor Tuning + I2C 1MHz (2026-06-29)

- **Interactive governor tuned** for faster ramp-up and UI smoothness:
  - `go_hispeed_load`: 90 → **75** (boost sooner)
  - `hispeed_freq`: 998400 → **1209600** (boost straight to max)
  - `min_sample_time`: 50ms → **30ms** (faster freq step-down)
  - `timer_rate`: 30ms → **20ms** (more frequent eval)
  - `above_hispeed_delay`: 25ms → **20ms**
  - `boostpulse_duration`: 60ms → **80ms** (longer touch boost)
  - `target_loads`: more aggressive curve (200MHz:37→25, ... 1209MHz:90→80)
  - Persisted via 120s reapply work (survives Android power HAL override)
- **I2C touch bus (QUP5)**: 400kHz → **1MHz** (`msm8916-common-15399.dtsi:105`). Driver already supports 1MHz divider table — only DTS change needed
- Sysbench CPU: 2286 → **2373 ev/s** (+3.8%)

## Build #368–#357 (Reverted)

### schedutil Governor Experiment — Reverted (2026-06-29)

**schedutil governor backported** from mainline concept. Uses scheduler UTIL_EST demand → frequency: `freq = max_freq * util / 1024`.

**Problems discovered:**
- **sleeping-in-softirq**: `__cpufreq_driver_target()` may sleep (mutex) → called directly from timer softirq → BUG splat. Fixed with workqueue deferral.
- **cpufreq sysfs hang**: `del_timer_sync()` on BSS-zeroed uninitialized timer (`timer->base = NULL`) → `spin_lock_irqsave(0)` infinite spin. Fixed by initializing timer before GOV_START.
- **UTIL_EST = 0 for CPU-bound tasks**: Tasks that never sleep have zero UTIL_EST → governor selects `policy->min` (200 MHz). Added `nr_running` fallback → per-CPU `nr_running=1` maps to only 25% max → still too slow.
- **schedule_work_on() starvation**: Work queued on busy CPU never runs → frequency never updates. Fixed with unbound `schedule_work()`.
- **Still 3× slower than interactive** (805 vs 2362 ev/s sysbench) even after all fixes.

**Root cause**: UTIL_EST only updates on task sleep/wake. CPU-bound workloads never sleep → zero signal. The HMP scheduler has no PELT-like continuous load tracking → any frequency scaling based on UTIL_EST is fundamentally broken for compute-bound tasks on this kernel.

**Conclusion**: schedutil cannot replace interactive on this kernel without backporting full PELT (thousands of lines). **Reverted to interactive governor** as default. schedutil remains available as an opt-in for testing.

### What's left in the code:
- `CONFIG_CPU_FREQ_GOV_SCHEDUTIL=y` stays enabled
- schedutil governor module remains compiled
- `sched_cpu_util()` function stays exported for future use
- **Default governor remains `interactive`**
- Switch manually: `echo schedutil > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor`

## Build #356

- **UTIL_EST**: Exponentially Weighted Moving Average (EWMA) of task utilization, backported from Linux 7.2-rc1. Prevents frequency collapse during brief task sleeps (~100 lines, `kernel/sched/fair.c`)
- **sched_min_granularity**: 750µs → **500µs** (runtime 1.5ms, 33% shorter time slices)
- **sched_wakeup_granularity**: 1ms → **250µs** (runtime 750µs, 4x faster wakeup preemption)
- **GENTLE_FAIR_SLEEPERS**: thresh >>=1 → **>>=2** (50% less sleeper credit, fairer scheduling)
- **CPU undervolt**: -25mV → **-50mV** on corners 3-8 (533-1209 MHz)

## Build #355

## Build #354

- **Removed fake CPU frequencies 1363 MHz and 1497 MHz** from `apcs_pll_freq[]`, DTS speed-bin tables, cpufreq-table, cpu-to-dev-map, and CPR corner map
- **Confirmed**: VCO=1 writes correctly (bit 28=1 in USER register) but PLL physically caps at ~1.2 GHz — CPU performance identical at 998, 1209, 1497 MHz (sysbench: ~1480 ev/s)
- **Max CPU frequency**: 1209600 kHz (corner 8)
- **GPU OC 620 MHz** preserved: uses RCG mux+divider from GPLL2, different mechanism from CPU PLL, likely real
- Updated hispeed_freq from 1497600 → 1209600

## Build #351

### Adreno Idler + Wakelock Blocker (2026-06-28)

- **Adreno Idler**: GPU stays in NAP (clocks off, rail on) for N idle cycles before deep sleep. Reduces SLEEP→ACTIVE thrashing during light GPU use. Sysfs: `gpu_idler` (on/off), `gpu_idler_idleworkload` (cycles, default 10)
- **Wakelock Blocker**: `/proc/wakelock_blocker` — block known battery-draining wakelocks (`+name` to add, `-name` to remove). 15 wakelocks blocked by default: wlan_wake, wlan_rx_wake, wlan_ctrl_wake, wlan_ipa, wlan_pno_wake, netmgr_wake, IPA_WS, qcom_rx_wakelock, ss_route_work, radio-interface, qcril, ims_socket_wake, CNE_WAKELOCK, cne_wqe_wake, power_policy_wake
- GPU sysfs permissions raised to 0666 for userspace write access
- Auto-enabled at boot via Cinnamon_Active Step 6–7

## Build #350

### Dynamic Fsync (2026-06-28)

- **vfs_fsync_range hook**: `/proc/dynamic_fsync` (1=skip fsync, 0=normal). Battery > 30% and not charging → skip fsync (faster I/O). Battery ≤ 30% or charging → normal fsync (data safe)
- Cinnamon_Active monitors battery every 60s, auto-toggles based on capacity
- Safe design: re-enables fsync on low battery, charging, or suspend

## Build #349

### TCP Westwood+ (2026-06-28)

- **TCP Westwood+** enabled as default congestion control (replaces CUBIC). Bandwidth estimation, better suited for WiFi/mobile (no slowdown from signal noise)
- Available: westwood, reno, bic, cubic, htcp
- Localversion: `-lineageos` → `-cinnamon`
- `lineageos_a37f_defconfig` removed, only `cinnamon_A37f_defconfig`

## Build #346

### Thermal Relax

- CPU frequency throttle: 60°C → **70°C**
- Core limit throttle: 80°C → **85°C**
- Hotplug/freq-mitigation: 94°C → **100°C**

## Builds #344–#345

### Panel Overclock 65fps + Touch Boost

- **Panel 60→65fps**: pclk=89,202,880 Hz, DSI VCO=356,811,520 Hz, bit clock=714 MHz (+6.6%). Modified BOE ILI9881C, TM NT35521S, Truly NT35521S panels. Reduced VBP/VFP
- **Fix**: removed `cont-splash-enabled` from panel DTS (blocking DTS timing)
- **Touch I2C 400kHz**: QUP5 bus 100kHz→400kHz
- **Synaptics**: `nosleep=1`, `report_rate=1` in wake function
- LM3630 PWM backlight: I2C dimming mode, `pwm-active=0`, PWM frequency hardware-fixed

## Builds #341–#343

### BIMC Clock OC Attempt (Abandoned)

- `of_clk_get_from_provider()` with `qcom,rpmcc-8916` DT node succeeded (build #341)
- `clk_round_rate(600MHz)=600MHz`, `clk_set_rate` returned success
- **But**: hardware still at 533 MHz — RPM firmware black box rejects higher rate votes
- BIMC timing registers S_SCMO at TrustZone locked — writes cause crash (build #342–#343, reverted)
- **Conclusion**: DDR cannot be overclocked on MSM8916

## Builds #338–#339

### DDR bw-tbl Override

- Added 5th entry `<4578>` to DDR bw-tbl, updated cpu-to-dev-map, added 5th GPU bus vector
- No effect on BIMC clock — RPM controls PLL, ignores higher rate votes

## Build #337

### Backlight Gamma Curve

- Replaced `MDSS_BRIGHT_TO_BL` macro with gamma-like curve (brighter at low brightness)
- `bl_min_lvl`: 30 → **1** (dimmer minimum backlight)

## Builds #334–#335

### Defconfig Cleanup

- Removed ~115 unnecessary config options: unused filesystems (btrfs, nilfs, jfs...), unused crypto algorithms (serpent, twofish, blowfish, camellia...), unused network protocols (DCCP, SCTP, DECnet, IPX, AppleTalk...), unused drivers (ISDN, telephony, WAN, CAN...)
- Cleaned DTS directory: kept only `cinnamon_A37f_defconfig` and 32 msm8916 DTS files
- Removed `CONFIG_MSM_FED` (faster panic)

## Build #333

### 120s Reapply: Fix Android Override of extra_free_kbytes

- **Scheduler reapply work (120s)** now also reapplies extra_free_kbytes=16384, swappiness=80, vfs_cache_pressure=50
- Fix: init writes extra_free_kbytes at ~50s via `sys.sysctl.extra_free_kbytes` property, overriding delay_trigger's write at ~39s. 120s reapply fires AFTER init so final value is ours.

## Build #332

### Kernel Defaults: Android-Proof Tunables

- **extra_free_kbytes**: default 0 → **16384** in `mm/page_alloc.c`
- **LMK 2GB tuning**: default minfree (1536,2048,4096,16384) → **(7168,8192,11776,15360,18944,22528)** pages in `lowmemorykiller.c`
- **Interactive governor defaults** in `cpufreq_interactive.c`:
  - go_hispeed_load: 85 → **75**, timer_rate: 20ms → **10ms**
  - min_sample_time: 80ms → **40ms**, target_load: 90 → **80**
  - io_is_busy: false → **true**, above_hispeed_delay: 10ms → **20ms**

### Notes
- Build #331 verified: L2 PC, GPU idle=500ms, RCU_BOOST_DELAY, io_is_busy survived Android override. extra_free_kbytes, LMK minfree, governor tunables were overwritten by power HAL/lmkd after boot.

## Build #331

### Performance Tuning: System
- **Disabled L2 Power Collapse**: wake latency 11ms → 240µs (msm8916-pm.dtsi, reduces jank after idle)
- **GPU idle timeout**: 80ms → 500ms (msm8916-gpu.dtsi, keeps GPU ready longer)
- **RCU_BOOST_DELAY**: 500ms → 100ms (defconfig, faster RCU callback processing)
- **extra_free_kbytes**: 0 → 16384 (delay_trigger, kswapd headroom against direct reclaim stalls)
- **Interactive governor tuning**: go_hispeed_load=75, min_sample_time=40ms, timer_rate=10ms, target_loads=80, io_is_busy=1, hispeed_freq=1497600
- **LMK 2GB tuning**: minfree 7168,8192,11776,15360,18944,22528 pages (moderate background killing)

## Build #330

### GPU Overclock 620 MHz
- Raised GPU from 465 MHz → 620 MHz (GPLL2/1.5, +33%)
- Added F(620000000, gpll2, 1.5) to oxili_gfx3d_465 frequency table
- Updated gcc_gfx3d_fmax: VDD_DIG_HIGH → 620 MHz
- Updated msm8916-gpu.dtsi: pwrlevel@0 and speed-config@2 both at 620 MHz

## Builds #328-#329

### CPU Overclock 1.5 GHz (1.4976 GHz, Corner 10)
- Raised corner 10 from 1.4 GHz → 1.4976 GHz (PLL L=78, 78 × 19.2 MHz)
- Added F_APCS_PLL entry (1497600000, L=78) to apcs_pll_freq table
- PLL fmax = 1.9 GHz at nominal voltage → safe margin
- Updated speed0-bin-v0, speed0-bin-v1, speed2-bin-v1 tables
- Updated cpufreq-table, cpu-to-dev-map with 1497600 kHz
- Verified stable: stress test held 1.5 GHz, no CPR errors

## Builds #325-#327

### DT2W Fix & Auto-Enable
- FIX: DT2W toggle wasn't applying immediately because `is_suspended` guard blocked I2C writes when screen on (build #325)
- FIX: DT2W reported KEY_F4 (143) which Android maps to WAKEUP but doesn't handle → changed to KEY_POWER (116) so Android wakes the screen (build #326)
- Auto-enable DT2W at boot 30s after boot via Cinnamon_Active Step 4 (build #327)

## Builds #319-#324

### CPU Overclock 1.3632 GHz (Corner 9)
- Added corner 9 to OPPO A37 regulator (msm8916-regulator-15399.dtsi)
- Extended cpr-corner-map, frequency-map, speed-bin-max-corners
- 1.3632 GHz from speed0-bin tables with bus bandwidth scaling
- Verified stable: 30s stress test, 45→53°C, no throttle, no CPR errors

### GPU Overclock 465 MHz + Efuse Bypass
- Raised pwrlevel@0 from 400 MHz → 465 MHz with bus bandwidth bump
- FIX: Efuse binning bypass — GCC clock driver forced 465 MHz table regardless of QFPROM (retail units locked to 400 MHz table)
- Verified: gpu_available_frequencies = 465 / 310 / 200 MHz
- GPU reaches 465 MHz under load via msm-adreno-tz governor

### GPU Governor Tuning (msm-adreno-tz)
- Reduced CEILING threshold from 50ms → 30ms (ramps to 465 MHz after ~2 frames instead of ~3)
- Event-driven (no polling): triggered on frame retire/idle → power efficient
- Touch boost: GPU wake 100ms on touch via adreno_input_work
- TrustZone algorithm handles frequency deltas securely

### Upgraded CCompress: Advanced Density & Low-Latency Execution
- **4-Bit Nibble Header System:** Upgraded from simple toggles to a high-density 4-bit metadata header (2 blocks per byte). Encodes 16 distinct compression states per block with minimal overhead.
- **4-Block Ring Buffer Recognition:** Rolling window tracking four most recent data patterns. Boosts match rates in Android's periodic memory structures.
- **Multi-Tier Delta Compression:** Specialized modes for 1, 2, and 3-byte differences (CINNAMON_MODE_DELTA1/2/3). Compresses "almost identical" blocks instead of falling back to raw data.
- **ARM64 NEON Hardware Acceleration:** Hand-optimized inline assembly for ARM64. Processes zero-checks, block matching, byte-diff counting using 128-bit SIMD.
- **Burst Run-Length Encoding:** ZERO_RUN (up to 65535 blocks) and REPEAT_BYTE_RUN modes collapse large memory buffers with near-zero latency.
- **Latency Mitigation:** Software prefetching via __builtin_prefetch; batched atomic updates reducing bus contention.

### CIO: Intelligence & Precision Architecture (Rewrite)
- **6-Tier Priority-Based Queuing:** Critical/Metadata > Read FG > Read BG > Write FG > Write BG > TRIM. Eliminates UI lockups (ANRs) and micro-stutters.
- **O(1) High-Performance Merging:** Limits merge depth to 8 entries per queue. Reduces CPU overhead and context switching.
- **Advanced FG/BG Balancing Engine:** Strict FG_STARVE_LIMIT with 16:1 dispatch ratio. Prevents background services from hijacking I/O bus.
- **eMMC-Specific Sequential Write Boost:** Intelligent detection for sequential write patterns maximizing eMMC cache efficiency.
- **Zero-Alloc Bit-Packed Metadata:** Bitwise operations embedding timestamps and priority into elv.priv pointer. Zero memory fragmentation, SMP safe.
- **Adaptive Latency-Aware Throttling:** Monitors disk health; throttles background batch sizes when latency exceeds 150ms to protect 50ms UI target.
- **FIX:** Bounded retry loop in cinnamon_set_pressure (was infinite with queue_lock held)
- **FIX:** CINNAMON_WRITE_EXPIRE corrected from jiffies to milliseconds
- **FIX:** Missing CRITICAL and TRIM/ERASE merge checks added

### frandom Driver
- ISAAC PRNG based, 24 MB/s throughput (vs urandom 2.8 MB/s, ~8.6x faster)
- /dev/frandom: fast PRNG for high-throughput random operations
- /dev/erandom: entropy-backed via get_random_bytes (get_random_bytes)
- Built as CONFIG_FRANDOM=y, chmod 644 at late-init

### Cinnamon_Active Scheduler Stability
- FIX: init.target.rc overrode Cinnamon_Active with "noop" on sys.boot_completed=1
- Added cinnamon_scheduler_reapply_work at 150s to re-apply after boot_completed
- Also sets sys.io.scheduler + persist.sys.io.scheduler properties
- Confirmed active after boot

### Other Improvements
- Removed CSleep to fix kernel power management conflicts
- WireGuard support: native kernel integration for faster, more stable VPN
- Native in-kernel root support for 'su' binary
- FIX: AnyKernel3 QCDT flash warning — CWD path corrected for AK3 environment
- FIX: Stale dtb file removed from zip (zip -r doesn't remove missing entries)

## Pending Ideas
- USB Fast Charge
- OverlayFS for Docker in chroot
- Sound Control (faux, increase max volume)
- ZRAM 1GB + lz4/zstd
- Input boost duration 40ms→80ms

## Current Problems
- Battery % during discharge is voltage-estimated, not measured — no hardware current sensor on this device (BQ24196 has no current ADC, no BQ27541 fuel gauge, VM-BMS IADC driver not present). Discharge current hardcoded at -450 mA
- Black screen if battery drop below 20% (not sure/user report)
