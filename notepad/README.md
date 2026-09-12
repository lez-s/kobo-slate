# Kobo notepad

An infinite-canvas notepad for the Kobo Libra Colour, reached from the dashboard
appliance in `../device/` by pressing the second page key. `notepad.c` is the
device loop; `store.c` and `canvas.c` hold the logic that can be tested on the
host, through the declarations in `notepad.h`.

## File format

A notebook starts with six bytes: ASCII `KNP1`, followed by little-endian `u16` version `2`. Records are append-only:

- `0x01` stroke: `u8 tool`, `u8 width`, `u8 color`, `u32 id`, `u32 npts`, then `npts` pairs of little-endian IEEE-754 `f32` canvas coordinates. Colors are black (`0`), red (`1`), blue (`2`), and green (`3`).
- `0x02` erase: little-endian `u32 id`.

Replay keeps complete records before an incomplete final record and discards that truncated tail. Version 1 files are loaded as black strokes and immediately compacted to version 2; version 0 and version 3 or newer are errors. Unknown opcodes and invalid complete headers are errors. Compaction rewrites the header and current live strokes; callers can evaluate `np_store_should_compact` at exit using their chosen dead-record threshold.

## Using it

**The pen draws, fingers navigate.** The panel reports which is which, so there
is no mode to switch and no heuristic to get wrong: the pen tip inks, the
eraser end rubs out whole strokes, one finger pans, two fingers pinch-zoom, and
a palm is ignored outright.

**Six controls sit along the top edge**, left to right: Wi-Fi, clear, undo, redo,
width, colour. The Wi-Fi control shows off, connecting, and on, and runs the
same `../device/wifi.sh` lifecycle used by the dashboard. A tap while it is
connecting is ignored until that operation finishes. Tap them with the pen or a
finger — a contact that starts on a control neither inks nor pans, for its whole
life, so sliding off one does nothing. Undo and redo grey out when their stack is empty. The barrel button
still cycles colour on a short press and toggles width on a long one, but only
while the tip is touching; see [Measured device
behaviour](#measured-device-behaviour) for why, and why these controls exist.

**Either page key returns to the dashboard** — "next app", of which there is
currently one. **The power key exits to Nickel**, and needs two presses within
five seconds; the first draws an `EXIT?` hint. The dashboard's own exit window
is deliberately longer, because it is *suspended* between presses and each one
costs a wake cycle, where this app is awake and reading evdev continuously.

**After ten minutes with no input the notepad hands the panel back** to the
dashboard, as if a page key had been pressed. Page one sleeps between cycles;
page two does not, so an open notepad would drain exactly the battery the
suspend loop exists to protect. `--idle-seconds N` overrides it and `0` disables
it — use a small value to test the behaviour rather than waiting ten minutes.
`--wifi-script PATH` replaces the Wi-Fi helper for host testing.

Wi-Fi is not forced down when the notepad exits: the dashboard owns the next
cycle's radio lifecycle, and leaving it on preserves the user's choice. Wi-Fi
left on costs battery until that next dashboard cycle; the ten-minute idle
timeout bounds the abandoned case.

The notepad reports its intent through its **exit status** and never reboots
anything itself: `0` means "back to the dashboard" (a page key, or the idle
timeout), `10` means "exit to Nickel". `dash.sh` owns the lock, the cleanup and
the reboot. Any other status is a failure, and `dash.sh` degrades to an ordinary
dashboard refresh rather than wedging.

**Clear archives, it does not delete.** The notebook is renamed to
`canvas-YYYYMMDD-HHMMSS.knp` beside itself and a new empty one takes its place,
so there is nothing to confirm and nothing to lose. Undo does not reach across a
clear.

### Ink and colour on this panel

While the pen is down the stroke is drawn **black** with the fast `A2` waveform,
and only on pen-up is a coloured stroke repainted over its bounding box with
`GLRC16`. That is not a stylistic choice: FBInk sets `EPDC_FLAG_FORCE_MONOCHROME`
for `A2`, and `DU` is documented "from any to B&W", so the two waveforms fast
enough to track a pen physically cannot show colour. Black strokes skip the
repaint and keep `DU`.

Kaleido colour is 150 DPI against 300 for black and is muted, which is why
The panel rules out thin coloured treatments
itself. Thin coloured ink here is correspondingly washed out; the thick width
is where colour actually reads.

Pan repaints at most every 200 ms with `DU` while the finger moves and once
cleanly with `GC16` on release. Pinch repaints **only** on release: zoom is a
step operation on e-ink, and intermediate frames are half-second updates that
buy nothing but flicker.

## Exporting a notebook

`tools/knp2svg.py` is both the escape hatch and the reference decoder — nothing
the device stores needs the device to read it back.

`tools/fetch-notes.sh` is the one-shot version: it pulls every notebook off the
device, renders each to SVG, and leaves both in iCloud Drive under
`Documents/KoboNote`, where they sync onward on their own.

```sh
sh tools/fetch-notes.sh
```

It only ever reads — nothing is removed from the device. Archived notebooks are
immutable once `clear` has renamed them aside, so they are fetched once and
skipped afterwards; `canvas.knp` is always re-fetched, being append-only and so
a superset of the previous copy each time. `KOBO_SSH_HOST`, `KOBO_NOTES_DIR` and
`KOBO_NOTE_DEST` override the defaults.

Expect it to fail when the device is between wakes: the dashboard suspends with
Wi-Fi off and is simply not there, which is the normal state rather than a
fault. Press a page key and run it again.

For a single notebook, or for PDF:

```sh
scp kobo:/mnt/onboard/.adds/kobo-dash/notes/canvas.knp .
python3 tools/knp2svg.py canvas.knp          # -> canvas.svg
python3 tools/knp2svg.py canvas.knp --pdf    # -> canvas.svg + canvas.pdf
```

It reads v1 and v2, tolerates a torn tail the same way the device does, and
crops to the ink's bounding box — an infinite canvas has no page to fit. The
PDF goes through headless Chrome,
so it adds no dependency; `CHROME_PATH` overrides the binary. The page is sized
to the drawing so a tall notebook comes out as one sheet instead of being
paginated across several.

## Tests

Run the native tests with:

```sh
make test
```

## Building for the device

`build.sh` copies the sources to the build host, compiles there, and brings the
binary back to `build/notepad`. The device needs a **statically linked armhf**
binary: it runs glibc 2.19 (the `/lib/libc-2.11.1.so` filename is misleading --
the banner reports 2.19) while a current Debian cross-gcc targets 2.36, so a
dynamically linked binary will not load. `-static` sidesteps the question, and
the notepad needs no networking, so none of the usual static-glibc caveats bite.

E-Ink refreshes go through **FBInk master**, statically linked. The tagged
release (v1.25.0, 2022) predates this device and has no `refresh_kobo_mtk()`;
master detects `Monza @ Mark 13` and enables the MediaTek path. Debian's
`gcc-arm-linux-gnueabihf` is enough -- FBInk's `CROSS_TC` only prepends a tool
prefix, so koxtoolchain is not required:

```sh
make -C FBInk staticlib CROSS_TC=arm-linux-gnueabihf MINIMAL=true DRAW=true
```

## Measured device behaviour

Everything here was measured on the target, not inferred. Several of these
contradict the obvious guess, and each one caused a real bug first.

**Geometry.** The framebuffer is 1264x1680, 32bpp, stride 5056, exact 1:1 with
no row padding. The touch axes are **swapped relative to the screen** and X is
mirrored (`fb0/rotate` is 1). Ranges are 1:1, so there is no scaling:

```c
screen_x = touch_y;            /* ABS_MT_POSITION_Y, 0..1264 */
screen_y = 1679 - touch_x;     /* ABS_MT_POSITION_X, 0..1680 */
```

**Input devices are discovered by name.** Pen and touch arrive on the Elan
panel (`event1`), the power key is separate (`bd71828-pwrkey`), and the page
keys are on `gpio-keys`; all three are discovered with `EVIOCGNAME`, never by
event number.

**`ABS_MT_TRACKING_ID` is re-sent on every report** -- measured at 1.02 per
`SYN_REPORT`, the same value repeated for the whole contact, `-1` only at lift.
Protocol B nominally sends it once, on change. Treating each repeat as a new
contact resets the stroke every report, and strokes come out as a row of
isolated dots that never reach the line-drawing path. Compare against the
current id.

**Draw only at `SYN_REPORT`.** X and Y are separate events and both are sent
every report. Acting on each as it arrives uses the other axis's stale value --
and on a contact's first report that value is the freshly zeroed 0, i.e. the
left edge, so every stroke began with a line reaching in from the left edge of
the screen.

**`BTN_TOUCH` does not mean contact.** It is 1 while the pen merely hovers.
Contact is `ABS_MT_PRESSURE > 0`:

| state | `BTN_TOUCH` | `ABS_MT_DISTANCE` | `ABS_MT_PRESSURE` |
| --- | --- | --- | --- |
| pen hovering | 1 | 15 | 0 |
| pen in contact | 1 | 0 | non-zero |
| finger in contact | 1 | *never reported* | 1088..3168 |

`ABS_MT_DISTANCE` separates hover from contact for the pen, but is never sent
for a finger, which would leave a finger holding a stale 15 forever. Pressure is
the only signal correct for both. A pen lifted back into hover keeps its
tracking id, so a stroke must also end when pressure reaches 0 -- otherwise the
next touch-down is joined to the previous stroke by a long straight line.

**Tools and buttons.** `ABS_MT_TOOL_TYPE` is 0 finger, 1 pen, 2 palm (the
maximum really is 2, so palm rejection is free). The eraser end reports
`tool_type` **1**, not 2, and is distinguished by `BTN_STYLUS`; `BTN_TOOL_RUBBER`
never fires. `BTN_STYLUS` is set while the eraser end merely *hovers*, so it
means "the eraser end faces the panel", not "the eraser is touching".
`BTN_STYLUS2` is the barrel button. Both are device-global, not per-slot.

| `tool_type` | `BTN_STYLUS` | `BTN_STYLUS2` | pressure | meaning |
| --- | --- | --- | --- | --- |
| 1 | 0 | 0 | 0 | pen tip hovering |
| 1 | 1 | 0 | 0 | eraser end hovering |
| 1 | 0 | 0 | >0 | pen tip drawing |
| 1 | 1 | 0 | >0 | eraser end in contact |
| 1 | 0 | 1 | >0 | barrel button held, tip in contact |

**The barrel button only works while the tip is touching.** Measured: three
presses made with the pen hovering produced **zero** `BTN_STYLUS2` events, and
the first press made in contact produced one immediately. The MPP link carries
the button state only during contact, so this cannot be fixed in software — it
is the whole reason the on-screen controls exist rather than being a nicety.

A related trap: the action fires on *release*, and a release only arrives while
the pen is within the digitizer's field. Press in contact, then carry the pen
out of range before letting go, and that release never comes. So the press
timestamp is taken on **every** press rather than on a 0-to-1 edge; otherwise
the flag sticks, the next press does not re-stamp, and the one after that
measures its duration against a timestamp minutes old — a tap read as a long
press, changing the width when the user asked for the next colour.

**Slots.** `ABS_MT_SLOT` has a maximum of 9, so ten contacts. It is not sent at
all while only one contact is present, and the current slot is **sticky across
reports** — the driver relies on that, emitting no `ABS_MT_SLOT` for a lone
surviving contact. With two fingers it does emit one per slot per report, only
ever 0 and 1. When one of two lifts, the survivor **keeps its slot** rather than
being renumbered, so a later single contact can perfectly well arrive in slot 1:
identify contacts by tracking id, never by slot index.

**The stylus is an active MPP pen and needs power** (gen 1 a AAAA cell, gen 2
USB-C). A flat one emits nothing whatsoever -- no hover, no `tool_type`, no
buttons -- which looks exactly like an unsupported device. `--finger-draws`
exists for that case: it lets a finger draw so the ink path stays testable.

## Measured performance

All figures from the device, not extrapolated from a desktop; synthetic
notebooks at 135 points per stroke, the measured mean of a real one.

| strokes | file | load | cull pass | eraser sample |
| --- | --- | --- | --- | --- |
| 1000 | 1.0 MB | 22 ms | 0.07 ms | 0.01 ms |
| 5000 | 5.2 MB | 108 ms | 0.33 ms | 0.03 ms |
| 20000 | 21 MB | 434 ms | 1.37 ms | 0.79 ms |

Storage is about **1.09 KB per stroke** and is not a constraint: 20000 strokes
is 21 MB against tens of gigabytes free.

Three things turned out **not** to be bottlenecks, each of which looked like
one: clearing the whole framebuffer is 1.6 ms, culling stays linear and trivial,
and a durable append -- the `fsync` at every pen-up, on FAT32 -- costs about
1.1 ms. That last one is why the durability is not traded away: it is paid once
per pen lift and is below notice.

**Rasterisation is what dominates**, and `draw_disc` is O(radius²) while the
radius scales with zoom. Stamping a disc at every pixel along a segment made a
stroke cost `steps * radius²` to cover an area of `steps * 2 * radius`. Full
repaint, before and after striding by half a radius in path-length units:

| | before | after |
| --- | --- | --- |
| 100 thin strokes, scale 1 | 28.7 ms | 28.5 ms |
| 100 thick strokes, scale 1 | 197 ms | 76 ms |
| 10 thin strokes, scale 8 | 127 ms | 22 ms |
| 10 thick strokes, scale 8 | **925 ms** | **125 ms** |

That last row is what made it worth doing: a pan repaint runs every 200 ms, so
nearly a second per frame at full zoom was a stall rather than a slowdown.

The thin pen at scale 1 -- the common case -- is **bit-for-bit unchanged**,
confirmed by pixel hash, because at radius 2 the stride is still 1.

The cost is scalloping. Discs spaced half a radius apart leave a periodic
narrowing along the stroke edge, measured at **up to 3 px on a 112 px wide
45-degree diagonal**, 2.7% of its width, with 1.16% less black area. It appears
only at high zoom with the thick pen. Tightening the stride to a third of the
radius would roughly halve it and cost about 60 ms per repaint, which is most of
the headroom under the pan cadence; that headroom is worth more than a pixel.

One subtlety worth keeping: the stride must be in **path-length** units. `steps`
counts the dominant axis, so an axis-based stride puts centres `sqrt(2)` further
apart on a 45-degree line than intended -- measured as 5 px of scalloping rather
than the 1.8 px the spacing was supposed to give.

## Testing on the device

The notepad owns the panel, so Nickel has to stop first. `timeout` bounds a run
that misbehaves, and a reboot puts Nickel back:

```sh
ssh kobo 'killall -TERM nickel hindenburg sickel fickel; sleep 2; timeout 90 /tmp/notepad'
ssh kobo reboot
```

Holding the power button for ~30 seconds is the fallback if the loop is wedged.
