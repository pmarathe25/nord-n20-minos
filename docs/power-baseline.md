# Battery baseline

On 2026-11-08, the phone was disconnected from USB power for 10 minutes with
Minecraft running, no players connected, Wi-Fi enabled, and the display at zero
brightness. The reported capacity changed from 100% to 99%; battery voltage
fell from 4.355 V to 4.340 V. The battery status was `Not charging` throughout.

The kernel reported `charge_now=0` throughout and `current_now` around 190–191;
the units and validity of the current reading have not been verified for this
vendor kernel. The short test began at full charge, where percentage readings
are coarse. It does not establish an average discharge rate or support a
prediction of weeks of runtime. A longer controlled test, ideally starting below
full charge and measuring actual energy, is needed. Do not run the battery flat
until a low-battery shutdown and recovery path have been verified.

An isolated phone-side software-renderer benchmark of the difficulty screen
averaged approximately 10.4 ms per frame without the glyph cache and 2.0 ms
with it over 20 frames each. This excludes framebuffer presentation, game load,
and whole-device power; it is not an estimate of battery savings.

With the updated dashboard installed and the display at zero brightness, a
separate 30-second observation recorded zero dashboard CPU ticks and zero
voluntary or involuntary context switches. The phone was charging during that
observation; it measures only the dashboard process, not the server, radio, or
total device power.

The MTP sandbox monitor also recorded zero CPU ticks and zero context switches
over a separate 30-second screen-dark interval while the phone was charging.
Its observed resident set was about 748 KiB; this excludes the responder and
does not measure whole-device power.

In separate 20-second observations on 2026-11-08 while the phone was charging,
the Wi-Fi manager recorded 36 voluntary context switches before its event-driven
update and 11 afterward. The second observation followed a scan and was not a
controlled power measurement. The new manager was also tested with simulated
connection changes and DHCP failure, and a live scan refreshed the state file.
The live server has `pause-when-empty-seconds=60`; its log confirmed that it
enters the paused state. While charging and with no players, a separate
20-second observation measured 0.04 CPU seconds across the Java process and
about 706 MiB resident memory. This does not measure total device power or
active gameplay, and it is not a battery-life estimate.
A phone-side comparison ran separate disposable, loopback-only servers with the
same seed, distance settings, 2 GiB maximum heap and a one-second idle pause.
With a 512 MiB initial heap, startup to ready took 26.05 s and paused RSS was
775,280 KiB; with 128 MiB, startup took 25.55 s and paused RSS was 682,528 KiB.
Paused CPU use was 0.06 and 0.07 CPU seconds over ten seconds respectively.
Each configuration was run once, in that order, with no players. Startup caches,
world generation and sampling variation can affect the comparison; it does not
establish performance under gameplay, garbage-collection pauses, or whole-phone
power. The live heap configuration was not changed.
In a later read-only snapshot, the live 512 MiB initial-heap server had about
720 MiB proportional resident memory, while the phone reported roughly 4.2 GiB
available RAM and no swap. This is one idle state, not a gameplay, garbage
collection, latency, or energy measurement; the lower initial heap remains a
candidate until it can be tested under representative player activity.

A later disposable-world test marked 64 chunks for force-loading with the
empty-server pause disabled. In one run per configuration, 512 MiB initial heap reached
ready in 25.55 seconds, used 888,852 KiB RSS after 30 seconds of work, and
consumed 76.89 CPU-seconds in that interval. The subsequent 128 MiB run reached
ready in 26.30 seconds, used 716,936 KiB RSS, and consumed 81.31 CPU-seconds.
The benchmark did not obtain usable tick-time output, did not include players,
and ran the configurations in a fixed order. It therefore cannot establish a
latency or battery-life advantage, nor the statistical significance of the CPU
difference. The live heap was not changed. The benchmark is available as
`tools/benchmark-server-workload.py`; it creates temporary worlds and checks
that the disposable server's listeners are loopback-only before its workload.

After improving tick-output capture, a second pass ran the same workload in the
opposite order. The 128 MiB run reached ready in 26.55 seconds, used 712,052 KiB
RSS, consumed 76.82 CPU-seconds, and reported 4.8 ms average and 10.0 ms P95
tick time over a 100-tick sample. The 512 MiB run reached ready in 26.05 seconds,
used 884,524 KiB RSS, consumed 81.32 CPU-seconds, and reported 5.4 ms average
and 10.5 ms P95. Tick samples represent the end of the synthetic run, not
player-visible latency; two runs per heap remain too few to isolate cache,
thermal and ordering effects. The live heap remains unchanged pending a
representative play test.

After moving the executable bundle to root-controlled storage, a live
`smaps_rollup` sample reported about 746 MiB RSS and 745 MiB PSS for the JVM;
it used 0.06 CPU seconds during a separate 15-second observation. This is a
single, uncontrolled sample with the existing 512 MiB initial heap, not a
before-and-after benchmark or proof of unchanged energy use or latency.

In separate 15-second Tools-page observations while charging, the dashboard
used 4 CPU ticks before static-page redraw suppression and 1 tick afterward.
These short measurements include normal background activity and do not isolate
rendering or measure device power. Phone-side navigation and touch wake passed
after the change. Dynamic telemetry pages still refresh periodically; static
pages refresh for state, freshness, input and message changes.
A later 15-second Tools-page observation recorded 18 voluntary context switches
before state-file notifications and 13 afterward, with the phone charging.
The samples are too short to establish an energy saving. The dashboard closes
its state-file watcher while the display is dark. Subsequent work powers down
the panel and uses the power button to wake it; touch wake remains a fallback
only if panel power-down fails.
A phone-side software benchmark of the Wi-Fi keyboard compared full frames to
partial scanline updates: 5.65 versus 2.90 ms per frame, with 10,444,800 versus
4,700,160 bytes copied per frame. This used ordinary memory, not scanout or
whole-device power. In one phone test of 30 injected taps, dashboard CPU time
fell from 68 to 23 ticks. The samples are not a controlled energy study.
Pixel-equivalence tests and a live dummy-password edit/Show/Hide check passed.
Other pages still use full-frame updates, and tearing has not been measured.
The same bounded scanline approach was extended to pressed-button outlines on
all pages; actions that can change pages or state still cause a full redraw.
One phone comparison of 20 injected navigation taps recorded 55 CPU ticks
before and 40 afterward. Pixel-equivalence checks and a phone-side held-press
and drag-cancel check passed. This is not a whole-device power measurement.

## Dark-display comparison

On 2026-11-08, with USB disconnected, Wi-Fi connected, Minecraft paused with no
players, and brightness at zero, the normal tap-to-wake mode was sampled for
10 minutes. The panel remained powered in that mode. A temporary experimental
mode then powered the panel off and disabled touch wake for five minutes. After
discarding the first minute of each run, the median reported `current_now` was
191 and 112 respectively; the median current-times-voltage proxy was 41.3%
lower in the second run. All samples reported `Not charging`. Transient raw
measurement files are not retained in Git. This is one sequential comparison, not a
repeated crossover, and the modes differ in both panel and touch state. It
does not isolate touch-controller power, establish active-gameplay savings, or
prove a battery runtime. The vendor kernel's current units have not been
validated, so the raw values should not be treated as calibrated power.
The player-count column in those files is blank because the event-based player
list was stale; the no-player condition was checked separately. The collector
now also consults a fresh dashboard status without probing the game server.

A separate short CPU-load check increased the median current reading from 191
to 381, showing that this counter responds to load; it does not calibrate the
counter. The reported charge counter changed in coarse steps corresponding to
whole percentages, making it unsuitable for this short comparison. A longer,
repeated discharge test or a suitable external battery power analyzer would
provide stronger evidence.

The measured difference exceeds the requested 10% runtime-impact threshold
as an idle power proxy. Initial panel-off tests could not restore the visible
display with the power button, even after a modeset. Later temporary tests
successfully restored the display by closing and reopening the DRM connection;
the user confirmed that touch worked after wake. The repository dashboard
now uses this approach and retains touch wake if panel power-down fails. The
integrated build was tested on the phone: touch did not wake the powered-off
panel, and the power button restored the same page with working touch. The
identical build was installed and the power and touch check was repeated
successfully. The previous dashboard binary is retained on the phone as
`/usr/local/sbin/n20-dashboard.before-panel-off` for recovery.

During the temporary panel-off tests, the phone's kernel log reported
`tp_suspend` completing on display power-down and `tp_resume` completing on
restoration; the gesture setting read zero. The touch controller identifies
itself as Goodix GT9886. A [OnePlus kernel source branch](https://github.com/OnePlusOSS/android_kernel_msm-5.4_oneplus_sm6375/blob/c82ecefdca2ac82853a36fbda2cfdb4ca69ab5a4/drivers/input/touchscreen/oplus_touchscreen_v2/touchpanel_common_driver.c)
contains a panel-blank notification path that suspends touch and can select
its sleep mode. That branch has not been proven identical to the installed
kernel. The log supports that the driver's suspend path ran, but does not
prove the controller was electrically powered off or measure its own draw.
On a later boot with the current dashboard, the backlight was zero, double-tap
enable read zero, and the most recent touch-panel transition in the kernel log
was a completed `tp_suspend`. This again confirms the reported driver path,
not electrical power removal or the controller's individual energy use.

The `pn553` NFC driver was bound, but no NFC userspace service or device
handle was found and its wakeup counter was zero. A
[OnePlus source branch](https://github.com/OnePlusOSS/android_kernel_msm-5.4_oneplus_sm6375/blob/c82ecefdca2ac82853a36fbda2cfdb4ca69ab5a4/drivers/nfc/pn553-i2c/pn553.c)
drives the NFC enable GPIO low at the end of its normal probe check; the source
has not been proven identical to the installed kernel and the live GPIO state
was not directly measured. No NFC driver or GPIO changes were made.

Both CPU policies were observed using the `performance` governor and Wi-Fi
power saving was off. Subsequent comparisons of `schedutil` and Wi-Fi power
saving are recorded below; they do not establish performance under gameplay.
In a separate five-second observation while charging and with no players, CPU0
and CPU6 accumulated approximately 5.00 and 4.87 seconds respectively in the
kernel's `wfi` idle state, despite their performance governors. This short
sample shows that the governor does not prevent those cores from entering that
reported idle state; it does not quantify active-load power, deep idle, or the
savings from another governor.

## CPU governor and Wi-Fi power-saving comparison

On 2026-11-08 the phone was unplugged, with the panel powered off and
Minecraft reporting no players before and after the test. Five sequential
two-minute phases used the existing performance governor and Wi-Fi power
save off as a baseline, then `schedutil`, another baseline, Wi-Fi power save
on, and a final baseline. Only one setting changed in each experimental phase;
both original settings were restored. The first 30 seconds of each phase were
discarded, leaving nine samples per phase.

The three baseline median current-times-voltage proxies were 483,738,000,
483,405,000 and 483,072,000. The `schedutil` median was 453,128,000, about
6.3% lower than the mean of its two adjacent baseline medians. Wi-Fi power
save's median was 218,200,000, about 54.8% lower than the mean of its adjacent
baselines. The reported battery temperature fell from about 27.2 C to 25.9 C
over the phases. These short, sequential idle samples are not calibrated power
or a battery-life prediction, and they do not measure active gameplay.

In a separate same-network check immediately afterward, 25 one-second-spaced
ICMP probes had no losses in either mode. With Wi-Fi power save off, median,
95th-percentile and maximum round trips were 2, 3 and 5 ms; with it on they
were 110, 118 and 125 ms. This is not a gameplay benchmark, but the increase
is large enough that Wi-Fi power save was left off. The governor was likewise
left unchanged pending active-load and latency tests. The event-based player
list can remain stale when no management events arrive, so it must not be
assumed to be a current idle signal for automatic Wi-Fi policy changes.

A follow-up used 100 ICMP probes with a 50 ms pause between replies to check
whether sustained traffic reduced the power-save latency. With power save off,
median, 95th-percentile and maximum round trips were 2, 5 and 15 ms. With it
on they were 37, 49 and 77 ms; neither run lost a probe. The original Wi-Fi
setting was restored. This single burst suggests active traffic reduces the
delay compared with sparse probes, but does not eliminate it or represent a
multiplayer test.

A separate synthetic CPU-burst test pinned fixed PBKDF2 work to CPU6 and
measured 40 bursts per phase, with 200 ms between bursts. Two performance
governor baselines had median durations of 5.979 and 5.974 ms; `schedutil`
between them had a 19.059 ms median. The original governors were restored.
This tests short CPU bursts, not Minecraft ticks or player-visible latency;
these measurements should not be used
alone to choose a gameplay governor.

A follow-up used 30 of the same bursts per phase while temporarily raising
CPU6's minimum frequency under `schedutil`. Performance-governor baselines had
medians of 5.992 and 5.986 ms; `schedutil` with 1.2288 GHz and 1.6512 GHz
minimums had medians of 10.741 and 8.011 ms respectively. The original
governor and minimum were restored. A later three-phase, screen-off idle
comparison found median current-times-voltage proxies of 482,073,000 and
481,962,000 for the adjacent baselines, and 460,358,000 for the intermediate
setting, about 4.5% lower. Each phase lasted 90 seconds and seven samples after
the first 20 seconds were used. Its impact on Minecraft remains unknown.

The automatic middle ground enables Wi-Fi power saving while the server has no
TCP clients and disables it after a client connects. A temporary phone-side
trial switched off power saving during a real player session; the player
reported normal movement and interaction, and a 12-probe sample had a 3 ms
median round trip. It returned to power saving after disconnect. A separate
60-second idle sample while the supervised policy was active recorded a median
raw current of 51, compared with 50 in the earlier power-save-only phase; this
is a short, non-simultaneous comparison, not a calibrated overhead estimate.
In real-phone
fault tests the policy returned to low-latency mode when its worker or its
supervisor was killed. The policy was installed and verified to return to
power saving after a test connection closed. It can still add delay to the
initial connection, and one player session does not establish multiplayer
performance across networks.

A later check found that the policy also treats loopback connections to the
game port as activity: holding one open for two seconds changed Wi-Fi power
saving from on to off, and it was on again twelve seconds after the connection
closed. Ten local administration status queries took 7.5–12.0 ms each, with a
9.9 ms median. The 500 ms monitor can therefore occasionally observe a status
query as activity, but this test does not quantify its whole-phone energy cost.
Loopback traffic was not excluded: a local proxy or SSH tunnel can also carry
real players, and the policy cannot distinguish those from status requests by
address alone. Such connections should retain the low-latency behavior.

A separate 20-second idle observation of the installed Wi-Fi policy recorded
0.37 CPU seconds in its worker and no measurable CPU ticks in its supervisor
(100 ticks per second). The worker and supervisor had approximately 6.4 and
6.2 MiB proportional resident memory respectively. This is a short process
sample, not a whole-phone power measurement or an upper bound under load.

After the later UI reorganization, a 30-second screen-off observation while
plugged in recorded zero dashboard CPU ticks and zero voluntary or involuntary
context switches, matching a 30-second sample of the previous dashboard. The
backlight reported zero throughout both samples. These are process activity
checks, not a sensitive measurement of whole-device energy or proof of zero
power draw. A separate 15-second sample found 26 CPU ticks in the Wi-Fi policy
worker and 9 in the kernel USB-temperature thread (100 ticks per second).
The Wi-Fi policy polls for connections so it can turn power saving off when
players join; changing its interval would trade response time against idle
overhead. The USB-temperature thread is part of charging protection and was not
disabled. No additional power saving is claimed from this observation.

After the later charging, Wi-Fi and protected-server UI changes were installed,
a 45-second display-off sample of the running dashboard again recorded zero
CPU ticks and zero context switches; the backlight remained at zero. The
running executable matched the installed dashboard. This longer activity check
still does not measure whole-phone power or establish battery life.

After a later reboot, the Java process showed a high lifetime-average CPU
percentage shortly after startup. A 15-second sample reported
0.04 CPU seconds for Java, with the backlight off and Wi-Fi power saving on.
The lifetime average included startup; this short sample does not establish
whole-phone power consumption or battery life.

The Wi-Fi policy's descriptor scan was later changed to use `os.scandir` and
simple ASCII socket-name validation instead of constructing a `Path` and running
a regular expression per descriptor. Two 300-iteration phone microbenchmarks
measured roughly 1.02 ms CPU per scan before and 0.54 ms afterward, with the
same live socket set. The poll interval and fail-safe rules were not changed.
After installation, a loopback TCP connection turned Wi-Fi power saving off and
it returned on after disconnection; isolated policy and crash tests also passed.
One 15-second idle worker sample used 17 CPU ticks after the change, versus 26
in the earlier sample. These short, uncontrolled observations do not establish
whole-device energy savings or a battery-life increase.

A later idle check reported a five-minute load average above 5 even though
CPU pressure over the preceding ten seconds was zero. A thread snapshot showed
five kernel threads in uninterruptible sleep (`panic_flush`, `fb_flush`,
`ubootback_threa`, `shortc_thread`, and `usbtemp_kthread`). Linux load averages
include uninterruptible tasks, so this value alone is not evidence of five busy
CPUs or an optimization opportunity. The snapshot does not establish how long
each thread remains in that state or measure its energy use; no kernel or
charging-protection thread was disabled.

Profiling also found that the policy ran its port-setting regular expression on
every server-properties line. Filtering unrelated lines first preserved the
existing validation behavior. In two 1,000-read phone microbenchmarks, the
configuration read used about 0.083–0.084 ms CPU before and 0.042 ms afterward.
This is a small per-check reduction, not a measured battery-life improvement.
Policy integration tests passed, and a live loopback connection again switched
power saving off and back on after disconnection.

A later read-only peripheral check found the backlight, vibrator LED and MMC
indicator reporting zero brightness; the only rfkill device was WLAN. The
kernel's `sensor_feedback` and `bp_camera` threads each recorded zero CPU ticks
and context switches during a 15-second sample. These observations do not prove
that the underlying hardware consumes zero power. No sensor, camera, thermal,
or radio subsystem was disabled on the strength of this short check.

A read-only experiment compared the current `/proc/net/tcp{,6}` scan with a
Python `NETLINK_SOCK_DIAG` dump on the phone. Both detected an isolated IPv4
listener and its connected client, and both reported the live server idle.
Over 40 back-to-back queries, the existing scan used approximately 0.38 ms of
process CPU per query and the experimental netlink dump approximately 0.60 ms.
This is a short microbenchmark, not a battery-life measurement or full protocol
equivalence test. The netlink prototype was not installed and the existing
connection-monitor interval remains unchanged.

## Unused phone hardware inventory

On 2026-11-08, a read-only inventory found no userspace camera, sensor,
Bluetooth, GPS or telephony service, no open camera/video/IIO device handles,
no IIO or Bluetooth class devices, and no camera sensor platform devices bound
to drivers. The display rotator reported runtime-suspended. The vibrator and
MMC indicator reported zero brightness. These observations do not prove that
every sensor or power rail is electrically off; several devices report runtime
power status as unsupported, and Android firmware may control shared hardware.
No userspace process held the ADSP, CDSP or video subsystem device handles;
the modem subsystem had one userspace handle, held by the Wi-Fi startup helper.
This does not account for kernel-side references or prove a subsystem is off.

The GPU reported zero busy percent and no process had a `/dev/kgsl` handle at
the time of inspection; forced clock, rail and bus settings were all zero.
Its `force_no_nap` setting was one, matching a `qcom,no-nap` property in this
device's tree. The corresponding [OnePlus kernel source](https://github.com/OnePlusOSS/android_kernel_msm-5.4_oneplus_sm6375/blob/c82ecefdca2ac82853a36fbda2cfdb4ca69ab5a4/drivers/gpu/msm/kgsl_pwrctrl.c)
sets that flag from the property. This source branch has not been proven
identical to the running kernel, and zero reported GPU activity does not prove
zero power draw. The vendor device-tree constraint was not overridden.

The modem subsystem is deliberately held open by the existing Wi-Fi startup
sequence. Its cellular data interface was down, and a read-only QMI Device
Management query reported operating mode `offline`. In
[libqmi's definition](https://www.freedesktop.org/software/libqmi/libqmi-glib/1.32.0/libqmi-glib-DMS-enumerations-and-flags.html),
that mode has deactivated RF and partially shut down the modem. The query used
temporarily extracted tools and did not change modem mode. It does not measure
the modem's remaining power draw or prove that the modem subsystem can be
stopped without affecting Wi-Fi. Because this device's Wi-Fi startup explicitly
depends on radio helper processes and an open modem subsystem handle, do not
stop the subsystem or change firmware as a routine optimization.

The process retaining that handle was changed from Python to a BusyBox sleep
with the device held open on file descriptor 9. On the running phone the new
handle was verified before releasing the old one; Wi-Fi remained reachable and
power saving stayed enabled. A single `smaps_rollup` sample showed proportional
memory falling from 3261 KiB to 378 KiB for this helper. This is a memory
measurement, not evidence of a measurable battery-life improvement. After an
unplugged reboot the revised startup script started the BusyBox holder, Wi-Fi
returned, and Minecraft came online after an operator corrected the clock over
SSH and NTS synchronized. The separate unattended clock issue remains.
