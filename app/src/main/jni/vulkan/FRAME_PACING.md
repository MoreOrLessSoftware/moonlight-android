# Host frame timing pacing

How the "Sync to host frame timing" pacing mode decides which vsync each streamed frame is shown
at. It aims for even frame pacing at the lowest delay the network allows. There are two
implementations of the same rules:

| Renderer | Code | Vsyncs from | Shows frames by |
|---|---|---|---|
| Vulkan | `jni/vulkan/frame_pacer.{h,cpp}` (`FramePacer`) | `AChoreographer` callbacks | Presenting ahead for a vsync (`VK_GOOGLE_display_timing` or an aligned front buffer), or at the vsync callback |
| Direct (MediaCodec to SurfaceView) | `binding/video/HostFrameTimeline.java` | `Choreographer` callbacks | `releaseOutputBuffer(index, timestamp)` |

Any change to the rules should be made in both.

## What it relies on

- **Host timestamps are the host's present times.** Sunshine stamps each frame with the time the
  host desktop presented it, so the spacing of timestamps is the game's real cadence.
- **The host runs at (a whole fraction of) our refresh rate.** The client asks the host for its
  frame rate to the microhertz (`clientRefreshRateUhz`, corrected for the host's clock), so a
  stream at the refresh rate is one frame a vsync, and at half of it two vsyncs a frame. Other
  ratios still work, but their uneven holds are then inherent (see Limitations).

The pacer doesn't map host time onto our clock. Only the spacing of host timestamps is used.

## Terms

- **Vsync grid**: the last vsync seen plus whole periods. The period is measured from vsync
  callbacks (least squares over 480 vsyncs natively, mean over 240 in Java), since Android may
  report 144 Hz for a panel that really runs at 143.65.
- **Slot**: the whole number of vsyncs between host frames, from the median spacing of the last
  15 host timestamps. One at 144 fps on 144 Hz, two at 72 fps.
- **Ready**: when a frame could first be shown, which is its arrival (decoded) plus the present
  guard (how long before a vsync a frame must be committed, measured by `PresentScheduler`) plus
  the preset's headroom.
- **Room**: assigned vsync minus ready. Negative means the frame is too late for its vsync.
- **Wait**: assigned vsync minus (arrival + present guard): the delay the pacer adds, headroom
  included. Shown on the overlay and in the logcat summary.

## The rules

Each frame, as it arrives (`FramePacer::onFrameArrived()`, `HostFrameTimeline.onFrameDecoded()`):

1. **Start**: the first frame, or one whose host timestamp jumped backwards or more than 250 ms
   forwards, goes to the first vsync at or after ready. The schedule starts from there.
2. **Chain**: otherwise the frame goes to the vsync after the previous frame's, as many slots on
   as its host timestamp advanced: `round(carry + delta / slotPeriod)`, at least 1. Half of the
   rounding remainder carries to the next frame, so a timestamp stamped late followed by one on
   time (1.4 then 0.6 slots) doesn't land two frames on one vsync. Only half carries: in full, a
   host slightly off our rate would add or drop vsyncs by itself, on top of rules 3-5.
3. **Late**: if room is negative, the frame goes to the first vsync it can make, and the frames
   after it chain from there. The delay grows by a vsync only when a frame actually needs it.
   (One frame held a vsync long.)
4. **Wait cap**: if wait (`room + headroom`) is the preset's `maxWaitVsyncs` or more, the frame is
   moved sooner until it's under the cap: frames bunched up after a stall, cold-start decode
   inflation, or a host running fast.
5. **Give back**: if every frame for the preset's `pullHoldNs` had room of at least a vsync plus a
   quarter (`period / pullMarginDivisor`), the frame moves as many vsyncs sooner as held throughout
   the window. Any frame with less resets the hold. (With a frame a vsync, this frame replaces the
   one assigned that vsync: one frame skipped.)

Rules 4 and 5 never move a frame before the previous frame's vsync. A frame given the same vsync
replaces the previous one.

A host running slightly slow against the display shows up as an occasional late frame, and one
running slightly fast as an occasional pull: one uneven frame each per drift cycle, the least the
rate difference requires.

## Presets

| Preset | Headroom | Wait cap | Give-back hold | Give-back margin | Late streak to shift | Typical wait (144 Hz) |
|---|---|---|---|---|---|---|
| Minimum | 0 | under 1 vsync | 0 s | +1/16 vsync | 1 frame | p50 5-6 ms, max under 1 vsync |
| Lowest latency | 0 | under 2 vsyncs | 1 s | +1/16 vsync | 2 frames | p50 5.5-7 ms, max under 2 vsyncs |
| Low latency | 0 | under 2 vsyncs | 2 s | +1/16 vsync | 1 frame | p50 6.5-8.5 ms, max under 2 vsyncs |
| Balanced (default) | 4 ms | under 3 vsyncs | 3 s | +1/16 vsync | 1 frame | p50 about 13-14 ms, max under 3 vsyncs |

Defined in `JitterBufferPreset::forJitterBuffer()` and `HostFrameTimeline.applyPendingJitterBuffer()`.
They can be switched mid-stream from the overlay menu, which resets the schedule and counters so the
new preset starts afresh from the next frame.

Stored values from presets since removed are migrated when read
(`PreferenceConfiguration.normalizeJitterBuffer()`): Smooth to Balanced, Lowest latency and
Instant to Low latency.

## Vulkan renderer integration

- `onFrameArrived()` sets `FrameTiming::targetNs` to the assigned vsync.
- With present-ahead, the renderer commits each waiting frame for `plannedVsyncNs()`: its
  assigned vsync, or the next one if that has passed. Frames are committed at most
  `presentAheadVsyncs()` (one slot) ahead. A frame for the same vsync as the last committed one
  replaces it, which counts as skipped.
- Without present-ahead, `onVsync()` shows the newest waiting frame whose assigned vsync is this
  one or earlier.
- `maxQueued()` holds every frame that may be waiting out the headroom and the wait cap, plus two.
  With fewer, the renderer drops frames the schedule still means to show.
- Partial frames (PyroWave): `partialDeadlineOffsetNs()` sets the deadline to the frame's assigned
  vsync less the present guard and the 90th percentile of recent decode costs. A frame still
  arriving then is cut rather than shown late, so it doesn't move the schedule.
  Since late frames move the schedule a vsync later, frames are rarely behind it, and only those
  caught in a burst are cut: at 120 fps, about 5 a minute where the old pacer, which scheduled
  frames by a percentile of past transit times, cut 90-160 a minute and showed them blurred.
  The decode cost is each frame's own decode, from when the decoder was free for it: its last
  packet, or the frame before it being decoded if later. Measured from the last packet alone, it
  took in the wait behind frames that arrived together, and the percentile set every frame's
  deadline for that wait (on a Pixel 10 Pro at 120 fps, 35 ms before frames were due where a
  cut frame took 11 ms), cutting frames that would have arrived whole in time.
  Cutting can't help frames late because the decoder is slow: on a device whose GPU can't
  decode the stream's resolution and frame rate in time, frames back up in front of the decoder
  (see the `N` and `D` lines) whatever the deadline.

## PyroWave late frames setting

| Setting | Frames still arriving when due | Frames that lost packets shown with |
|---|---|---|
| Off | shown late | 90% of them |
| Balanced (default) | cut short with at least 30% in, 2 ms before the cut-off | 75% |
| Smoother | cut short with at least 20% in, 2 ms before the cut-off | 50% |

Set in `VulkanRenderer`'s constructor; `debug.moonlight.partial*` properties override them.
Smoother cuts no earlier than Balanced: with frames rarely behind the schedule, cutting earlier
(the old Smoother and Smoothest cut 3-5 ms early) only blurred frames that would have arrived
whole. Stored values from levels since removed migrate: Sharper to Balanced, Smoothest to
Smoother.

**Tried and dropped: cut assist.** The idea was to let the give-back rule move frames a vsync
sooner even when a few wouldn't arrive whole by then, counting on cutting them short. Replays
suggested 96-99% of frames could often go a vsync sooner. On devices it never paid off: what
kept frames from the earlier vsync was a busy decoder (a Pixel 10 Pro at 120 fps, where cutting
doesn't get a frame decoded sooner, and judging from packet times alone moved frames sooner and
straight back), a network burst or lasting slowdown affecting many frames, or the phase of the
host's frames putting a third of them at the cut-off. Never a steady few percent with late
final packets, the only case it helps.

## Direct renderer integration

The direct renderer runs a Choreographer thread in this mode only to feed vsyncs to
`HostFrameTimeline.onVsync()` (frame time less the app vsync offset). Frames are released as
they're decoded, stamped half a vsync before the present of their assigned vsync (assigned vsync
plus one refresh of present latency, less half a vsync). SurfaceFlinger shows a buffer at the
first vsync whose expected present is at or after its timestamp, so stamping mid-interval keeps
timing jitter from moving it to a neighbouring vsync. There's no present guard. Frames assigned
the same vsync are dropped by SurfaceFlinger, keeping the newest.

## Diagnostics

- **Overlay (Vulkan)**: wait, measured refresh rate, vsyncs per frame, and running counts of late
  frames and pulls.
- **Logcat, Vulkan** (`VulkanPacer`, every 10 s): frames, short/long holds, skips, wait, late
  and pulled totals. A preset switch logs `Jitter buffer changed to N`.
- **Logcat, direct renderer** (`com.limelight.LimeLog`, lines starting `Host frame timing`): the
  preset at start and on every switch, and every 10 s the frames, mean and max wait, late frames
  and pulls in that window, vsyncs per frame, measured refresh rate and preset. This renderer
  writes no trace. `adb shell dumpsys SurfaceFlinger --latency '<SurfaceView layer>'` gives the
  last 128 frames' requested and actual present times: each should be shown half a vsync after
  its requested time, and a vsync after the last.
- **Traces**: `adb shell setprop debug.moonlight.pacer_trace 1`, or `tools/pacer-trace.sh` (format
  in `pacer_trace.h`). In V lines, `locked` is whether a schedule is running, `scheduleDelayNs` is
  the newest frame's wait, and `slotParity`, `transitBufferNs` and `driftPpm` are 0. The `C`
  line's `jitterBuffer` is the preset at stream start, and `J` lines are switches.
- **Replay** (`tests/pacer_replay.cpp`): replays a trace's frames and vsyncs through the current
  pacer and prints uneven frames and the arrival-to-vsync wait. `--jitter-buffer low|balanced`
  replays with another preset. A replay of an unchanged pacer matches the device to within a frame
  or two, so it's reliable for comparing changes.

- **Simulation tests** (`tests/frame_pacer_test.cpp`): synthetic streams (matched and half
  rate, host clock drift either way, spikes, a stall, host gaps, a frame rate change, preset
  switches, and PyroWave frames with late packets cut short, or stuck behind a decoder near its
  limit), each with and without presenting ahead, checked against the properties above. Run
  them after any change to the rules. Build commands are at the top of each file.

When reading a trace, separate the causes of uneven frames:
- **Host gaps**: the assigned step equals the host timestamp step (the host didn't send a frame,
  common on the desktop and while loading). The pacer reproduces these faithfully.
- **Late frames**: assigned a vsync more than the host step. Arrival spikes.
- **Pulls**: assigned a vsync less than the host step.

## Tuning notes

Measured by replaying sessions streamed over Wi-Fi to a 144 Hz tablet:

- **Hold length**: at 72 fps, a 3 s give-back hold rather than 1 s cut uneven frames by about a
  quarter for about 1 ms more median wait. Longer holds (10-30 s) saved little more, since spikes
  come every few seconds.
- **Headroom**: 4 ms (Balanced) absorbs most lone spikes of a few ms. Bursts of 10-30 ms late
  frames get through any headroom tried, and 8 ms bought little more than 4.
- **Lower latency**: shorter holds (0.3-1 s) saved 0.1-0.5 ms of median wait for up to twice
  the uneven frames. Always using the earliest vsync (a wait cap of 1) made 4-100 times as many
  frames uneven. A smaller pull margin changed nothing.
- **Late credit** (letting the cap allow an extra vsync after a late frame): cut uneven frames
  during bursts by about 40%, but held latency higher afterwards. Balanced handles bursts better.
- **Where the remaining delay is**: present guard (about 2 ms), waiting for the next vsync
  (half a vsync on average, but set by where the host's frames happen to land against our
  vsyncs), and the extra vsync the pacer keeps when arrivals sit near a vsync.
- **Tried and parked: steering the host's phase.** Replays with every arrival shifted
  (`tools/pacer-trace.sh sweep`) found the phase worth 0-2.5 ms of mean wait at 144 Hz,
  depending on where it happened to land, with the lowest wait right before a cliff of late
  frames (never the ~7 ms a whole vsync would suggest). A controller asking the host to run up to
  400 ppm off its frame rate (branch `phase-steer-wip` here, in moonlight-common-c and in this
  fork's Sunshine) worked in simulation but not on the host: Sunshine moved the game through its
  RTSS frame limit, and every RTSS profile reload, even with the limit unchanged, freezes the
  game's presents for 40-49 ms and lands its phase anywhere in the frame interval. Steering
  needs a frame limiter whose rate can change without restarting its timing.

## Limitations

- **Balanced counts a frame that only eats into its headroom as late**, and moves the schedule
  later, though the frame would still have made its vsync. That keeps the headroom in place for
  the next spike, but right after a pull it can cost an uneven frame Low latency wouldn't have.

- **Frame rates that aren't a whole fraction of the refresh rate** (120 fps on 144 Hz, or an
  uncapped game) can't be shown evenly. The pacer follows the host's cadence with whatever uneven
  holds the ratio requires, and they count as late frames and pulls.
- **Direct renderer**: assumes one refresh of present latency, and keeps the vsync period from its
  own measurement, so a refresh rate change mid-stream isn't picked up.
