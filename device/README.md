# Kobo device loop (Phase 0)

This directory is the source of truth for the device-side Kobo dashboard loop.
It targets `/bin/sh` on the Kobo Libra Colour and keeps all persistent state on
the USB partition at `/mnt/onboard/.adds/kobo-dash/`.

## Loop

`dash.sh` turns the frontlight off, brings up the verified MTK Wi-Fi sequence,
fetches a manifest, and writes a new `8494080`-byte RGBA frame only when
`data_sha` changed. It then tears Wi-Fi down, derives a relative RTC alarm from
the manifest's `now_epoch`, and suspends. A charging device uses `sleep` instead
of suspend because this MTK kernel can hang while plugged in.

Every cycle also stamps one partial-update line at the bottom of the panel:
wake time, cycle number, and battery percentage (`+` when external power is
connected). It is drawn outside the `data_sha` gate, so it is the only proof
from the device alone that a wake actually happened on a cycle where the data
did not change. Battery can only be shown from here — the panel is rendered on
the server, which cannot know this device's charge, and the mailbox transport
is read-only in the other direction.

Remote failures keep the existing frame and use 5, 10, then 20 minute
backoff. A cached manifest remains the schedule during a server outage. Local
failures are counted and, outside development mode, reboot to Nickel after the
configured threshold. A wake before the saved alarm is a key press, and what
happens next depends on which key it was — see [The two
pages](#the-two-pages).

The Phase 0 default is `DEV=1` and `FAKE=1`. In this mode the loop does not
suspend or power-cycle Wi-Fi; it uses the generated fake manifest/frame and
plain `sleep`, so it can be exercised over SSH. Set `DEV=0` only for the manual
acceptance step while physically holding the device.

## Launching

`nm-kobo-dash` is the NickelMenu entry; deploy it to
`/mnt/onboard/.adds/nm/kobo-dash`. It adds a **Dashboard** item to the home
menu that spawns `dash.sh`, the same way the existing KOReader entry spawns
`koreader.sh`.

`dash.sh` stops Nickel (`stop_nickel`, using koreader.sh's kill list minus
`dhcpcd`/`dhcpcd-dbus`) and owns the panel.

Page one is an appliance, not an app: it has no touch UI, and it sleeps between
cycles. Page two is the opposite — a touch-and-stylus app that stays awake while
it is open. That is why it hands the panel back after ten minutes with no input
of any kind, not only on a page key: a notepad left face-up on a desk would
drain exactly the battery the suspend loop exists to protect. The timeout
returns to page one like a page key does — never to Nickel, because an
unattended device must not reboot itself.

### The two pages

The page-turn keys are not interchangeable: each one selects a page.

**Back (`KEY_F23`, 193) refreshes the dashboard.** The panel draws
`refreshing...`, fetches the current frame, and repaints; the stamp line then
ends with `via page key`. The schedule restarts from the new manifest, so the
next automatic wake is one interval after the manual one.

**Forward (`KEY_F24`, 194) opens the notepad** — page two, an infinite canvas
for the Kobo Stylus, built in [`kobo/notepad`](../notepad/README.md). It owns
the panel while open. Either page key switches to the next/previous app, so
from the notepad either `KEY_F23` or `KEY_F24` returns here. On the way back
the loop repaints the dashboard from the cached frame immediately, without
waiting for a fetch, and then runs an ordinary cycle. A missing or failing
notepad binary degrades to the plain refresh above rather than wedging the
loop.

Anything other than an unambiguous 194 refreshes, which is the safe direction:
a misread costs one refresh, never a panel stuck in an app.

There is no refresh while the device is on charge. Suspending hangs this MTK
kernel when plugged in, so the loop uses a plain `sleep` instead, and nothing
can wake a sleep.

### Exiting

**Press the power key, wait about three seconds, then press it again.** The loop
reboots into Nickel. The dashboard uses a 30-second double-press window because
the device is suspended between presses and each press costs a wake cycle; a
press only registers while suspended. The notepad is awake and reads evdev
continuously, so it uses a separate five-second window for two quick presses.

The pause matters. A press only registers while the device is *suspended* —
during the two or three seconds the loop is awake drawing the "EXIT?" hint,
nothing reads evdev, so a press there is silently dropped. Pressing twice in
quick succession therefore does nothing, which reads as a dead button. The
power key deliberately does *not* refresh, so the second press is never queued
behind a fetch.

A single press just draws the hint and goes back to sleep, so it costs one wake
cycle and nothing else. `EXIT_PRESS_WINDOW` (default 30s) bounds the dashboard
pair; the notepad's pair is bounded by its five-second awake-app window.

On the dashboard, page presses never count toward the power-key exit, so keys
knocked in a bag refresh the panel rather than rebooting it. In the notepad,
either page key switches back to the dashboard instead.

### Telling the keys apart

Both the page keys and the power key wake this device, and they are separate
wakeup sources: `/sys/devices/platform/gpio-keys` and the PMIC's
`bd71828-pwrkey`, each reporting `power/wakeup = enabled` with its own event
counter. The loop samples both counters either side of every suspend, and the
one that moved names the key. Measured: a page press adds 2 to the gpio-keys
counter (press and release), a power press adds 1 to 3 to the pwrkey counter,
and the two never move together.

An earlier note here claimed `gpio-keys` reported `wakeup=n/a` and that every
early wake was therefore the power key. That was wrong, and it had a
consequence: two page presses used to reboot to Nickel.

Anything other than an unambiguous page press is treated as a power press. A
page press misread costs one refresh; a power press misread costs the only way
out of the loop short of a 30-second forced reset.

The counters name the *device*, not the key, and both page keys share one, so
the loop also keeps an evdev reader open across each suspend — the process is
frozen rather than closed, so the press that caused the wake is still queued on
resume. The two page keys report 193 and 194 (`KEY_F23` and `KEY_F24`), and
that code is what chooses between the dashboard and the notepad — see [The two
pages](#the-two-pages). The capture holds *every* `EV_KEY` record queued during
the suspend, presses and releases alike, so the loop takes the last press
(`value == 1`) whose code is one of those two and ignores the rest.

Holding power for ~30 seconds still force-resets the device, and remains the
fallback if the loop is wedged.

It is deliberately **not** wired to a boot hook. That is what makes long-pressing
the power button a reliable recovery path: any reboot lands back in Nickel. The
cost is that the dashboard does not restart itself after a reboot or a flat
battery — relaunch it from the menu.

`DEV=1` skips `stop_nickel` entirely so the loop can be exercised over SSH.

## Transport

The LAN mailbox is the only enabled transport: when `iwgetid -r` matches `SSID`,
`fetch.sh` pulls from `LAN_ENDPOINT`. Anything else is reported as
`remote unavailable`, and `dash.sh` applies its remote-failure backoff while
keeping the current frame on screen.

`LAN_ENDPOINT` is served by the router, which also decides how long this device
sleeps next: it stretches the manifest's `next_wake_epoch` to a longer interval
while nobody's phone is on the LAN. Nothing is pushed to a suspended device
to make that happen — `manifest_delay()` already takes the schedule from the
manifest, so the new interval applies the moment this loop next asks for one.
See [`docs/mt3000-kobo-mailbox.md`](../../docs/mt3000-kobo-mailbox.md).

The `dbclient` ssh path is **opt-in and off by default**. `SSH_HOST` is empty in
`config`; while it is empty the ssh branch is never attempted. To enable it, set
`SSH_HOST` by hand to the publishing host's current public address — that address is
**ephemeral**, so it goes stale after any VM restart and fails silently. There is
no on-device UI for this: NickelMenu entries are static text, so changing the
address means editing `config` over SSH.

## Install and fake frame

From the repository checkout:

```sh
python3 kobo/device/fake_frame.py /tmp/fake-frame.raw.gz
gzip -dc /tmp/fake-frame.raw.gz | wc -c  # 8494080
scp kobo/device/{wifi.sh,fetch.sh,dash.sh,suspend_probe.sh,fake_frame.py,README.md} kobo:/mnt/onboard/.adds/kobo-dash/
# config ONLY on a first install, and never afterwards -- see Configuration below
scp kobo/device/config.example kobo:/mnt/onboard/.adds/kobo-dash/config
ssh kobo 'chmod +x /mnt/onboard/.adds/kobo-dash/*.sh /mnt/onboard/.adds/kobo-dash/fake_frame.py'
scp /tmp/fake-frame.raw.gz kobo:/mnt/onboard/.adds/kobo-dash/fake-frame.raw.gz
```

Page two needs its binary in the same directory. It is cross-compiled rather
than checked in, so build it first — see
[`kobo/notepad`](../notepad/README.md):

```sh
sh kobo/notepad/build.sh
scp kobo/notepad/build/notepad kobo:/mnt/onboard/.adds/kobo-dash/notepad
ssh kobo 'chmod +x /mnt/onboard/.adds/kobo-dash/notepad'
```

The path is defaulted inside `dash.sh` as well as in `config.example`, because
`config` on a device is local state that deployment never overwrites — a device
installed before page two existed would otherwise source a config with `NOTEPAD`
unset. Notebooks live beside it in `notes/` and are never touched by a
deployment.

Copying a frame to `/dev/fb0` and refreshing is:

```sh
gzip -dc /mnt/onboard/.adds/kobo-dash/fake-frame.raw.gz > /dev/fb0
/mnt/onboard/.adds/koreader/fbink -s -f
```

`fbink` is used only for refresh; both installed builds cannot decode images.

## Configuration

`config` on the device is **device-local state, not a deployable artifact** —
the same relationship this repo keeps between `.env` and `.env.example`. It
holds `DEV`, the SSID, intervals and failure thresholds, all of which get tuned
on the device during testing.

`config.example` is the template. Copy it to `config` on a **first install
only**. Never deploy it over a device's existing `config`: doing so silently
reset a hand-set `DEV=0` back to `1` in the middle of a battery test, which
skipped `stop_nickel` and made the run look like "the frame appears, then
Nickel repaints over it a few seconds later" — a misleading symptom that cost
a full test cycle to track down.

## Recovery

The loop is not attached to an automatic boot hook in Phase 0. To recover the
device, **long-press the power button and reboot into Nickel**. A STOP file is
not a recovery mechanism because the sleeping device has Wi-Fi off and cannot
receive that file. Do not stop Nickel or exercise the real suspend path during
remote development.
