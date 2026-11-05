# phxboot mock harness: test the boot daemon logic WITHOUT a reboot

Created on 2026-10-24 to remove the dependency on physical reboots of the box. The daemon
`tools/phxctl/phxboot-navi31.sh` is plain shell that orchestrates the whole arm sequence and its fail-safes, and that is
WHERE the bugs that burned reboots lived (the `2>/dev/null`, the `!WindowServer`, the blocking-gate deadlock, the
kickstart hang, and on 10-29 the AGDC oid race). This harness runs the daemon's REAL logic against a fake root, in under
a second, with no hardware.

The scripts under test (the daemon and the scripts it runs) are in `tools/phxctl/`; the mocks reach them as
`$HERE/../../tools/phxctl/...`, so they run from any directory. `p0prefetch_test.sh` needs a register dump of the target
machine (`P0PF_DUMP=<file>`) and is skipped without one.

## How it works
- `tools/phxctl/phxboot-navi31.sh` IS the daemon that gets installed as `~/gpu/phxboot-navi31.sh`: every path can be
  overridden by a `PHXBOOT_*` environment variable (default = the production value), so on the box it behaves exactly as
  before while the harness redirects everything to a fake root. Edit it, run the mocks, then deploy.
- `run.sh` generates fake shims (ioreg, metaltest, mesboot, sysctl, who, killall, pgrep, nvram, launchctl,
  **shutdown [RECORDED, never reboots]**, sleep [near-instant, injects crashes], sync), driven per scenario by `SIM_*`
  settings, and runs the daemon with asserts (`expect_rc / expect_log / expect_nolog / expect_killall /
  expect_no_killall / expect_reboot / expect_no_reboot / expect_nvram / expect_nonvram`). Modelled on Cobalt64
  `tools/pc/m6-stage2-sim/run-scenarios.sh`.

## Run
```
./run.sh                       # every scenario (must be all green before deploying the daemon; 11-03: wsp_* / silent_* / late_* added)
bash bootflags_mut.sh          # 16 mutations of hotplug_boot / gamma_boot, each must be CAUGHT by run.sh (11-01)
bash wsprefs_mut.sh            # 17 mutations of the offer-gate handling, the v4 leftover restore, dispovr and the silent-crash watches (11-03, v7)
bash bootrate_mut.sh           # mutations of the boot-rate (aocboot / aocboot1) path in the daemon, b15_boot.sh and p0prefetch.sh (11-03 v8: +19 p0solo)
bash p0prefetch_test.sh        # p0prefetch.sh incl. PHXP0_SOLO (27/27)
bash relight_mut.sh            # 50 mutations of b15_boot.sh's Acer relight (watch2 rl_*), each must be CAUGHT by the rl_* scenarios it names (11-04; RLALL=1: all rl_*)
python3 hotplug0_mut.py        # 8 mutations of the daemon's hotplug0_boot (~/gpu/phxboot-hotplug0), each must be CAUGHT by run.sh hp0_* (11-04)
# b15test.sh runs the same under sh and bash: never 'VAR=x func' (bash POSIX mode keeps VAR after the call)
bash watchsrv_mut.sh           # 11 mutations of b15_boot.sh's watchdog read server path (srv_start/sget/bgof, w2read, follow0, watch1), each CAUGHT by b15test.sh (11-03)
PAR=1 bash relight_mut.sh      # any runner / suite: PAR = how many at once (default 2), see "Cost knobs" below
bash b15diff.sh OLD NEW [VAR=v] # decision diff of two b15_boot.sh versions over every b15test.sh scenario (logs, calls, writes, stub call counters)
bash phxctl_serve_test.sh      # host test of `phxctl serve` (the read server; no kext needed: register items answer empty, sysctl items real) (9/9)
./run.sh -k                    # keep the work dirs (to debug a FAIL)
./run.sh happy user_logged     # only the named scenarios
DAEMON=/path/other.sh ./run.sh # test another daemon (used for mutation tests)
bash b15test.sh                # the idle-power display recipe script (must be 132/132 before deploying b15_boot.sh; 11-03 with the AOC-alone p0solo cases, the 16 rl_* Acer relight cases, the read-server srv_* cases and spawn_count = processes per poll)
bash m7test.sh                 # the M7 second-monitor boot script m7_boot.sh (must be 8/8)
bash dettest.sh                # det.sh -p <pipe> show/clear/set/rollback (must be 11/11)
bash b21guard_mut.sh           # 15 mutations of the login-window guard (b21_on / b21_off and their hooks), each CAUGHT by run.sh b21_* (11-04)
```

## Cost knobs (11-04: the suites ran the MacBook fan at max)
- `PAR=<n>`: how many run at once. **run.sh and b15test.sh**: a full run splits its scenarios round-robin over n shards
  (default 2; `PAR=1` = the old serial loop); every scenario still gets a fresh world, the output is printed in scenario
  order once all are done, one RESULT line. b15test.sh defaults to `PAR=1` when `B15ONLY` or `B15STOP` is set.
  **Mutation runners** (`*_mut.sh`, `hotplug0_mut.py`): n mutations at once (default 2), each one's mock serial and at
  `nice -n 19`; the CAUGHT / MISSED lines come out in mutation order. Parallel only buys wall time: the CPU work is the same.
  `PAR=1` everywhere = the minimum heat at any moment.
- What makes a run cheaper (same checks): the fakes read their state files with shell builtins instead of a `cat` per
  read (b15test.sh's phxctl.bin register reads and read server, the sysctl / cgmode / engine-state stubs; run.sh's pgrep,
  sysctl, ioreg, sleep shims); the fake read server sources an engine-state stub instead of exec'ing it; a scenario that
  `B15ONLY` skips reuses one scratch world and never runs the script; run.sh copies a prebuilt fake root per scenario.
- Mutation runners run only what a mutation can reach: `relight_mut.sh` names the rl_* scenarios per mutation (`RLALL=1`:
  every mutation against all rl_*, the old check) and stops at the first FAIL; `bootrate_mut.sh` (b15 mutations) and
  `watchsrv_mut.sh` stop at the first FAIL (`B15STOP=1`) and the b15 mutations name their scenarios too. A mutation whose
  named scenarios all pass is reported MISSED, never silently skipped: widen its glob (or `RLALL=1`) if a code change moves
  the catching scenario.
- Measuring a run: `/usr/bin/time -l` misses the fake read server (b15_boot.sh leaves it as an orphan, ~2/3 of the old
  b15test.sh CPU). `B15CPU=<file>` makes each fake server append its `times` there at exit (sum it with the rusage).
- Default for a check before deploying: `bash b15test.sh` + `./run.sh` (PAR=2) and the mutation runner of the area touched.

Measured 11-04 on the MacBook (`nice -n 19`, one suite at a time; CPU = user+sys incl. the orphaned fake servers; spawns =
processes created, from a pid sampler), same pass / CAUGHT counts before and after:

| suite | before: wall / CPU s / spawns | after: wall / CPU s / spawns |
|---|---|---|
| b15test.sh (sh), 159 | 350 s / 214 / ~254k (serial) | 75 s / 71 / 78k (PAR=2); 152 s / 75 / 78k (PAR=1) |
| run.sh, 150 | 142 s / 62 / 53k | 70 s / 55 / 47k (PAR=2); 138 s / 57 / 47k (PAR=1) |
| relight_mut.sh, 50 | 4896 s / 3080 / 3.47M | 70 s / 80 / 83k |
| bootrate_mut.sh, 56 | not run (27 full b15test.sh runs: est. ~2.6 h / ~5900) | 222 s / 190 / 221k |
| watchsrv_mut.sh, 11 | 86 s / 48 / 45k | 9 s / 10 / 8k |
| bootflags_mut.sh, 16 | 38 s / 14 / 10k | 19 s / 12 / 9k |
| wsprefs_mut.sh, 17 | 60 s / 12 / 10k | 45 s / 11 / 9k |
| hotplug0_mut.py, 8 | 64 s / 27 / 23k | 31 s / 26 / 21k |
| b15diff.sh (2 x b15test.sh) | est. ~700 s / ~430 | 187 s / 170 / 187k |

run.sh's remaining cost is the daemon itself and one `/bin/sleep 0.01` per `sleep` shim call (kept: it paces the
daemon's wall-clock loops and its background jobs).

## Scenarios in run.sh (each one = a failure mode that used to cost a reboot)
| scenario | injects | expects |
|---|---|---|
| happy | everything OK | SUCCESS, killall once, no reboot |
| metal_stderr_guard | metaltest logs ONLY to stderr | Metal detected (guards the 2>&1 fix) |
| metal_fail | Metal never comes up | FAIL, **no killall**, reboot |
| phxfb_fail | PhxAMDFB does not take the panel | FAIL, no killall, reboot |
| nmxaccel_fail | NMXAccel does not start | FAIL, no killall, reboot |
| user_logged | `who` returns a user | FAIL "a user is logged in", **no killall** (safety), reboot |
| killswitch | `~/gpu/phxboot-off` exists | "kill switch present", nothing done, no reboot |
| mark_recovery | pending mark present (previous attempt did not finish) | reverts to GOP + creates the kill switch, no reboot |
| not_desktop | no phxboot-desktop | "no desktop arm: GOP", no killall |
| crash_loop | 3 crashes after the killall | FAIL "crashed/hung", reboot |
| ws_norestart | WindowServer does not come back | FAIL "no WindowServer after the restart", reboot |
| agdc_fail / agdc_never | AGDC arm fails / never arms | FAIL, no false SUCCESS, reboot |
| agdc_late | the debug.phxagdc oid appears late (10-29 race) | the arm is retried: "AGDC up after", SUCCESS |
| squat_happy, squat_noauto, squat_killswitch, squat_mark, squat_metal_fail, squat_user, squat_crash | the phxfb-squat boot path | the same fail-safes plus the nvram re-arm / clear |
| vcn_off, vcn_up, vcn_fail | VCN bring-up flag absent / OK / failing | desktop SUCCESS in every case; vcn lines as expected |
| b15_off, b15_on, b15_missing, b15_nodesk | phxboot-b15 flag absent / present / script missing / not a desktop boot | b15_boot.sh started only on a desktop boot with the flag |
| nodesk_no_boot_arm | no desktop arm | phxfb-boot nvram not set, no killall |
| hotplug_off/on/bootarg/bootarg_like/fail/nohead1/nom7/nodesk (11-01) | ~/gpu/phxboot-hotplug absent / present / boot-arg -phxhotplugoff / a look-alike boot-arg / the write refused / bind failed / no M7 / not a desktop boot | `debug.phxhotplug=2` written exactly once only with the flag on a desktop boot with head 1 bound, after SUCCESS + `done` (WindowServer restarted) and before `b15: started`; one log line with rc + read-back; a refusal is logged and the boot goes on |
| gamma_off/on/pref_true/pref_absent/nocdpref/busy/busy_stuck/fail/nohead1/nodesk (11-01) | ~/gpu/phxboot-gamma, the cdpref answer, EBUSY once / always, EPERM | `debug.phxgamma=1` once at the same stage (EBUSY: up to 3 writes, anything else: 1); the pref is logged, **never set** (any `cdpref set/delete` fails the scenario) |
| flags_both, flags_term | both flags; a TERM during the crash watch | one write each, hotplug before gamma before b15; TERM = no write |

## b15test.sh (10-29)
`sh tests/mock/b15test.sh` runs tools/phxctl/b15_boot.sh (the idle-power display recipe the daemon starts with
~/gpu/phxboot-b15) against a fake phxctl / register file: happy, dpm (flag phxboot-b15-dpm), underflow, dpm_underflow,
step_fail, not_gop, missing.
11-03 rl_* (16): the Acer relight after a hot-plug event (fake sysctl for debug.phxhotplug_stat / debug.phxscdc /
debug.phxpipe2_switch): dark at 165 -> 144 -> lock x2 -> 165; SCDC fine / Acer at 144 / no event = nothing (no SCDC read
without an event or at 144); 165 fails -> back at 144; no lock at 144; engines busy / hot-plug pending = wait; a mode
macOS set meanwhile = stop. `B15ONLY='rl_*' bash b15test.sh` runs only those.

## m7test.sh (10-30)
`bash tests/mock/m7test.sh` runs tools/phxctl/m7_boot.sh (light = pipe-1 ladder survey/ping/stream before the AGDC
arm; bind = debug.phxhead2=1, plane, debug.phxfb1_flip=1 before the one WindowServer restart; undo) against fake sysctl /
pipe2 state: 8 cases incl. bind_flip_refused and old_kext. run.sh adds the daemon side: m7_off, m7_on, m7_light_fail,
m7_bind_fail, m7_missing (flag ~/gpu/phxboot-m7; order proof light before the AGDC write, bind after it).

## dettest.sh (10-29)
`bash tests/mock/dettest.sh` runs tools/phxctl/det.sh (`-p <pipe>` show / clear underflow stickies / set /
rollback DET size) against a fake phxctl: 11 cases incl. clear_pipe1 and bad_pipe.

## Validity (it has teeth)
Mutation test confirmed: re-introducing `metaltest 2>/dev/null` turns `happy` and `metal_stderr_guard` RED with the exact
diagnosis ("missing Metal device ready; killall NOT issued; reboot WAS issued"). Likewise `agdc_late` fails on daemon
502300ef and passes on 65be28b4. So if someone re-introduces a real bug, the harness catches it at the desk, not in a reboot.

## Limits (what the harness does NOT cover)
- It tests the **daemon logic (ordering, fail-safes, branches)**, NOT the kext (C++), IOKit matching, or the real
  behaviour of CoreDisplay / WindowServer / Metal. For those, the next step is a **Tahoe VM (QEMU/OSX-KVM) with
  snapshots** (as RX4Darwin does).
- The shims model the hardware in a simplified way (claim -> NMXAccel + PhxAMDFB appear; Metal = 1 after the claim).
  They reflect what we observed, not the silicon.

## First-WindowServer fix (PhxAccel offer gate) and silent-crash watches (11-03)
- `fixtures/wsprefs-aoc144.plist` = the box's /Library/Preferences/com.apple.windowserver.plist of 11-03 (display serial numbers set to 1).
- Shims: `defaults export|import` on the fake prefs ($SIM_STATE/wsprefs.plist), `log show` (kernel "Corpse allowed" lines the
  mocks recorded), `pgrep` reads the WindowServer pid from $SIM_STATE/wspid (`killall` sets 23456 and saves the prefs the
  restarted WindowServer reads as prefs-at-kill.plist).
- SIM_WSMODEL=1 (prefs_world): the first WindowServer (pid 12345) reads the prefs ONCE, before the daemon runs ($ST/ws0-snapshot.plist);
  on the arm's `debug.nvaccelfb=1` write a one-display set in THAT copy away from id 1 / 60 kills it (pid 1021, a corpse, no
  .ips) only if head 0 OFFERS id 2: its list armed before the claim (phxpipe0_modes) and the offer gate not closed.
- SIM_OFFER_GATE=1 = a PhxAccel with the head-0 offer gate (src/vermilion-accel/accel/phxoffer.h): the gate closes at the claim (unless -phxfb0open in
  SIM_BOOTARGS), `debug.phxfb0_offer=1` or a resync to id 2 opens it; without it the oid does not exist.
- `session_saves`: a logged-in session's display save (SkyLight copies the user's sets, AOC alone at 144, into the system prefs).
- Two-boot scenarios: `boot1` (log in boot1.log) + `reboot_world`, then `go` = boot 2 with the checks.
- SIM_WS_SILENT=N (120 s watch), SIM_WS_LATE_SILENT / SIM_WS_LATE_LOGOUT=N (watch_hangs, bounded by WATCHMAX).

| scenario | expects |
|---|---|
| ws_crash_oldkext | no gate: the first WindowServer dies (logged "gate absent"), prefs untouched |
| gate_fix | gate: survives; the gate opens before the killall (killed=no), prefs untouched |
| gate_session_race / gate_cold_twice | the v4 shutdown race (a session save between the boots) / two boots: survives |
| v4_race_repro | the same race without the gate: dies (the box fail) |
| gate_bootarg / gate_no_list / gate_open_fails / gate_resync_opens | -phxfb0open = old behaviour / no list = nothing to restore / a failed open logged / a resync opened it |
| v4_leftover / v4_leftover_import_fail / v4_leftover_moved | a v4 stash goes back once (flag reported unused) / kept on a failed import / left when the set moved |
| silent_crash_loop / silent_crash_once | 5 / 1 WindowServer deaths in the 120 s watch, no .ips: FAIL + reboot / SUCCESS, logged |
| late_silent / late_logout | 2 pid changes after `done` with / without a kernel corpse: revert to GOP / not counted |
| one_aoc / one_aoc_refused / one_aoc_resync_bad | AOC alone (m7 light rc 1, pipe 1 never lit), boot rate 144: `b15_boot.sh aocboot1` before the restart, resync id 2, gate verified / aocboot1 refused: 60, no resync / resync not id 2: aocback |
| one_aoc_m7off / one_aoc_noflag / one_aoc_no_solo | AOC alone without ~/gpu/phxboot-m7 / ~/gpu/phxboot-b15 / the `debug.phxpipe0_solo` oid (an older PhxBringup): logged, the AOC stays at 60, no write (11-03 v8) |
| two_unaligned_no_solo | pipe 1 lit but not phase-aligned: never the one-display path |
| one_acer | Acer alone on head 0 (single mode, no list): "head 0 has no 60/144 mode list", gate opened, SUCCESS |

## Login-window guard (SHOWFULLNAME, b21_*)
Fake domain `$FR/Library/Preferences/com.apple.loginwindow.plist` (no SHOWFULLNAME; `sfn_set` = the user's own value); the
`defaults` shim does read / write -bool / delete on it with PlistBuddy and logs each call to `$ST/lw.log` (verb key value
killed= termed=); SIM_LW_BROKEN_AT_TERM=1 = after the TERM `defaults` reaches no cfprefsd (read absent, write a no-op);
SIM_LW_FAIL=<verb> refuses that verb. Helpers: expect_sfn (true / false / absent at the end), expect_b21orig, expect_lw / expect_nolw.

| scenario | expects |
|---|---|
| b21_start | kill switch boot: ON at the daemon start, before anything else; key true, user value 'absent' recorded |
| b21_happy / b21_user_true / b21_user_false | OFF right before the restart (killed=no): absent = deleted / the user's true kept / the user's false written back |
| b21_gop / b21_metal_fail / b21_crash_loop / b21_late_revert | GOP boot, a FAIL before or after OFF, the watch_hangs revert: key true at the end |
| b21_shutdown / b21_attempt_term | the shutdown TERM after SUCCESS (rc 143 as before) / during the crash watch: ON, key true |
| b21_term_nocfp_false / b21_term_nocfp_true | TERM with no cfprefsd: the plist written directly; the user's value read from the plist |
| b21_flag_off / b21_flag_off_left | ~/gpu/phxboot-b21guard = 0: no defaults call at all (TERM too); a guard left ON by an earlier boot is reported |
| b21_two_boots | shutdown ON, next boot: start reads '1' (what the GOP loginwindow read), OFF deletes it |
| b21_off_fails | delete refused: OFF FAILED logged, the user's value kept in the .orig file |
