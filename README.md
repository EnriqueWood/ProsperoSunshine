# ProsperoSunshine

![ProsperoSunshine](docs/images/banner.jpg)

A native, Sunshine-compatible game-stream host for the PlayStation 5. Add your console to Moonlight and stream
the PS5 screen, game audio and controller input over your LAN.

> **Status: 0.4.1 ([changelog](CHANGELOG.md)).** Pairing, H.264, HEVC and HDR from 360p up to 4K at 30 or 60 fps, game audio, controller, keyboard and mouse input work end to end
> with Moonlight on PC, in the browser (Moonlight Web) and on iPhone. Sessions of 30 minutes and ten reconnects
> were tested without errors on firmware 13.00. Firmware 10.20 was tested by a contributor at 1080p. Users report streams on
> 9.60, 10.40, 11.40, 12.70, 13.40 and 13.60, not measured here. A second device can join a running stream as another player, in its own codec, dynamic range or resolution up to 1080p. Not every client or game has
> been tried.

## How it works

ProsperoSunshine builds [Sunshine](https://github.com/LizardByte/Sunshine) (pinned commit, small documented
patches) for the console and replaces the parts that talk to PC hardware:

- **Video:** the console's hardware encoder (Avcap2) delivers H.264 or HEVC, 8 bit or HDR, straight into Sunshine's packetiser.
- **Audio:** game audio as PCM from the same capture service, encoded by Sunshine's Opus path. Stereo, or 5.1 and 7.1 with
  the experimental Surround switch.
- **Input:** a native virtual DualSense per Moonlight gamepad, with touchpad and motion sensors, and a virtual keyboard and
  mouse for Moonlight's keyboard and mouse. A device without a controller plays with its keyboard and mouse as a
  controller of its own.
- **App list:** "PS5 Screen" and the console's installed games, with their covers. Sunshine's web UI is replaced by the PS5
  app and a small [web page](#web-page) of the host's own, which is off until you set a login.

Details: `docs/adr/001-sunshine-port.md`. Roadmap: `ROADMAP.md`; verified state: `STATUS.md`;
reverse-engineering notes: `docs/` (German).

## Requirements

- PS5 firmware: **13.00 and 10.20 are tested** (10.20 from 360p to 1080p, where the encoder takes no bitrate; its picture quality
  follows the bitrate set in Moonlight instead, and it sends well below it). Other versions are
  untested; the host refuses them only if its system-library lookups fail (`docs/firmware-porting.md`, German). Firmware
  9.60 has its own capture record, measured by a contributor; there the app can't save settings yet, as a title's sandbox
  has no `/data` on that console.
- A homebrew environment with an **ELF loader** (for example one listening on port 9021). The app also needs
  [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus), which registers titles from `/data/homebrew/`.
- No PS VR session while streaming.
- A wired or good wireless LAN, and a Moonlight client in the same network.

## Install

**Option A: the PS5 app "ProsperoSunshine" (`PPSA47989`).** The app carries the host and offers start/stop,
autoload through the Payload Manager, settings and PIN entry. Install it from ProsperoStore, or copy the folder from
`PPSA47989.zip` (GitHub release) to `/data/homebrew/`. Details: `app/README.md`.

| Home | Settings |
| --- | --- |
| ![Host status, start and stop, pairing](docs/images/app-home.jpg) | ![Host settings](docs/images/app-settings.jpg) |

**Option B: send the ELF manually.** Send `prospero_sunshine.elf` (from a release, or `out/` after building) to your
ELF loader. The host keeps its state (identity, paired clients, `sunshine.conf`) in `/data/prosperosunshine/`.
To replace a running host, stop it first (app: "Stop host"); a second ELF cannot bind the ports and exits, and the
old host keeps running.

## Pairing

1. In Moonlight, pick the console, or add it by its IP address (`<console-ip>`), and start pairing. Moonlight shows a PIN.
2. Enter the PIN in the ProsperoSunshine app on the PS5 (option A). The app then asks for a name for the device, because Moonlight
   sends the same one, `roth`, from every device. Press Options to keep the suggested name.
3. Without the app, approve it through the control socket from the same machine, or from the LAN if you enabled it
   (see below): `printf 'PIN 1234\n' | nc <console-ip> 47995`.

Pairing survives host restarts. To rename a device or take its access away, select **Paired devices** on the app's home page. The
list also shows the mode each device last asked for. Removing a device ends its running stream, and it has to pair again to come back.

While someone streams, the app's home page shows **Streaming now**: each device with its name, address, codec and HDR,
resolution and frame rate, bitrate, how long it has been streaming and how many controllers it has. Its list also shows
what really reaches each device: the bitrate it gets, the round-trip time, and how often it lost a picture and asked for a
new one, which points to a weak connection. Select a device to end its stream; the game and the other devices' streams go
on, and the device stays paired and can connect again.

## Streaming

Start **"PS5 Screen"** in Moonlight to stream what the console shows, or pick a game to start it as well. The first time, the
console asks **"Who's using this controller?"** inside the stream; answer it with your Moonlight controller. A device given
its own profiles doesn't get the question ([Profiles per device](#profiles-per-device)).

By default the host never closes a game, because that could lose unsaved progress. Quitting in Moonlight ends the stream, and
the game keeps running on the console. If another game is open, in front or behind the settings app, picking a game fails
(Moonlight reports error 409) and starts nothing: close the running game first, on the console or in a "PS5 Screen" stream,
then pick the game again. Media apps aren't listed; start those through "PS5 Screen".

**Closing the game from Moonlight:** with **Quit closes the game** on in the app's **Settings**
(`close_game_on_quit = 1` in `sunshine.conf`, off by default), the host behaves like Sunshine on a PC for the game the stream
started, or the one already in front when you picked it. After the stream it stays listed as
running while it is in front, so Moonlight offers to resume or quit it, and picking another game asks to quit it first.
Quitting closes it as "Close Game" on the console does, and unsaved progress is lost. On Moonlight for PC, the stop button on
the running game quits it; so does Ctrl+Alt+Shift+E during the stream, which doesn't ask and also closes Moonlight, or any end
of the stream when "Quit app on host PC after ending stream" is on. With that option off (Moonlight's default),
Ctrl+Alt+Shift+Q only ends the stream and the game keeps running. On iOS the running game's menu has "Resume App" and "Quit
App", and another game "Quit Running App and Start"; on Android "Resume Session", "Quit Session" and "Quit Current Game and
Start". A quit from another device also closes the game and ends the stream there, as with Sunshine on a PC. "PS5 Screen"
closes nothing.

**PS button:** Moonlight's on-screen controller on a phone has none, so holding Select for half a second sends it. A
short press of Select sends Create when you let go, which means Create cannot be held. To get a plain Create button
back, set `back_button_timeout = -1` in `sunshine.conf`; a value of 0 or more is the hold time in milliseconds.

**Several controllers on one device:** every controller on a Moonlight client is a player of its own. The host's first
controller plays as the console's user, and for each further one the console asks "Who's using this controller?" once it
is first used (on a device with its own profiles, it joins as the next one instead): a controller the client announces but never uses (Steam Input's virtual one next to the real one, the
on-screen controls) doesn't appear on the console at all. Two people on one PC need one
Moonlight with both controllers connected, not two clients. Measured here with the test client (two pads on one device),
and a contributor played with two controllers on one Moonlight client on 10.20.

**Only the host has PS and Create.** The host is the device whose controller came first while no other was connected; its
controller plays as the console's user. Every other player is a guest who can only play: their PS and Create presses are
ignored, also the "hold Select" PS, so a guest can't leave your game, open the control center, reach the console's settings
or broadcast your screen. The role belongs to the host's device, not to the first slot: when the host leaves, nobody else
gets it, and the host's device takes it back when it reconnects. Once every controller is gone, the next device to connect
is the host. To give PS and Create to everyone, turn on **PS for all players** in the app's Settings and restart the host.
On a device with [profiles of its own](#profiles-per-device), once you choose the [console menus](#console-menus), the
controller's profile decides instead of its role.

## Web page

The host can serve a small web page, so a phone or PC in your network can do what the app does without leaving the game:
see the host's status, version and firmware, enter the PIN of a pending pairing, end a device's stream, rename or remove
paired devices, choose the console profiles each device plays as and which of them reach the console's menus, change the settings, read or download the end of `sunshine.log` and `host_diag.log`, get the same
diagnostic report as the app's (ready to paste into an issue), and restart the host.
It is off until you set a login, and nothing listens on its port before that.

1. In the app, open **Settings**, choose **Web page** and set a user name and a password (8 to 128 characters). The page
   starts right away. Turn it off there again; the login stays stored for the next time.
2. Open `https://<console-ip>:47990` in a browser (the **Base port** plus one, if you changed it). The page has its own
   self-signed certificate (made on the console, renewed by the host), so the browser warns once; accept it for this
   address. On an iPhone in Safari that takes a few taps: **Show Details**, **visit this website**, then **Visit Website**.
3. Sign in. A session ends after 12 hours without use, or when the login changes. With **Remember me on this device**
   (off unless you tick it) the browser stays signed in for 30 days from that sign-in, also across host restarts and a
   closed tab. The host keeps only a SHA-256 hash of such a session's token, in `/data/prosperosunshine/web_remembered`.
   Changing or forgetting the login in the app, turning the page off, and **Sign out** on the page end every remembered
   session, so signing out on one device also signs out a lost phone.

- The page answers only the LAN and Tailscale addresses (100.64.0.0/10); every other address gets 403. **Don't forward
  TCP 47990 to the internet.** Over [ps5-tailscale](#without-port-forwarding-tailscale) it is reachable only if 47990 is
  among the ports it passes. ps5-tailscale forwards its clients from the console itself, so the page sees all of them as
  127.0.0.1: they share one counter for failed sign-ins, and the log names 127.0.0.1 for each of them.
- After 5 failed sign-ins from one address, the page waits 30 seconds before the next try, doubling up to 15 minutes; after
  20 from all addresses together, every address waits the same way until the next good sign-in. Failed sign-ins are
  logged with the address, never the password. The password is kept only as a salted PBKDF2-SHA256 hash in
  `/data/prosperosunshine/web_login`.
- Settings changed on the page apply when the host starts again. After **Save** the page offers to restart it, and until
  then a banner says the saved settings aren't applied yet. The Experimental switches stay in the app, where they are
  checked against the firmware.

## Profiles per device

Each paired device can be given the console profiles it plays as. Lend the console to a friend and they play on their own
profile, with their own saves, and nobody at their place can load, overwrite or delete yours. The [web page](#web-page) asks
right after pairing, and **Devices** changes it later; the app only says that it is set there.

- **Only these profiles:** each controller of the device joins as the next listed profile, and the console doesn't ask
  "Who's using this controller?". The host signs the profile in if needed, and the console shows "<profile> joined the
  party". A controller beyond the device's profiles doesn't join, and the console says so. The device plays through
  its controllers only, or its keyboard and mouse as a controller of their own when it has none
  ([Keyboard and mouse as a controller](#keyboard-and-mouse-as-a-controller)); they never reach the console's shared
  keyboard and mouse, which belong to no profile: the console would ask "Who's using this mouse?" and let anyone pick.
- **Watching only:** with none ticked, Devices marks the device **Spectator**. It streams but doesn't play: the console
  shows "<device> joined as a spectator", and its controllers, keyboard and mouse don't join. Give it a profile while it
  streams and the console tells it at once; its next press joins as it, without reconnecting.
- **Can choose freely** (marked Risky): as before, the console asks and any profile can be picked, yours too. Devices
  paired before profiles start like this, so nothing changes until you choose. Overview warns about them; **Don't show
  again** stops it for those devices, and Devices still marks them Risky. Tap a device's badge in **Devices** for why it
  is risky and **I trust this device**: it then shows **Trusted**, the notice leaves it out, and its badge has **Stop
  trusting**.

**Switching profile:** a player on a device with more than one profile switches to its next free one with
**Ctrl+Backspace** on a keyboard, or by holding **L1+L2+R1+R2** for two seconds on a controller. The console shows "<old>
passed the controller to <new>", and that controller starts there next time. Once it has gone round all of them, or after
a refused switch, it waits 20 seconds before it can switch again, so nobody fills the screen with notifications.

**Changing a device's profiles while it streams:** the stream goes on. Profiles added are there at once, and the console
says so: a spectator joins with its next press, a player can switch to them. A controller playing as a profile taken away
leaves it, and the console says so: it only watches until a button joins it as another of the device's profiles, if one
is free. The console takes four signed-in users at most, so the host
signs out the profiles it signed in once their last controller leaves (not the one in front on the console), and when it
stops.

**Guests:** the host can't add a Quick Play guest yet; the web page shows it greyed out. Until it can, create guest
profiles on the console, up to four ("Guest 1" to "Guest 4"), as the console takes four controllers at once, and give
them to the devices you lend. A device with all four can play with four controllers and reaches no one else's profile.
Unlike Quick Play, these profiles keep their saves, shared by everyone who plays on them; a friend who wants to keep their
own gets a profile of their own.

### Console menus

**Devices → Console menus** chooses which profiles reach the console's own menus from Moonlight. Lend the console and a
friend plays their game, but can't open Settings, update the system (which also ends the jailbreak) or sign in to PSN.
On a device with profiles of its own:

- A controller playing as a chosen profile has PS and Create, also F1 and F2 of the
  [keyboard's controller](#keyboard-and-mouse-as-a-controller) and the PS of a held Select. A controller playing as another
  profile has neither, even the host's. The device's own keyboard plays as the profile of its first controller, and for a
  profile without the menus its two keys into them do nothing: Print Screen (the Create menu) and Pause (the control
  center).
- Only a device with at least one chosen profile gets **"PS5 Screen"** in its app list, and the host refuses to start it for
  any other device, as a client can ask for an app by its id without seeing it. Any device can start a game from its list,
  and leave it with Quit when **Quit closes the game** is on.
- While no game is in front (the home screen after a game ends, an app of the console's, a media app or homebrew), the
  controllers, keyboard and mouse of other profiles do nothing, and a press says why: "<profile> can't use the console's menus. Start a game
  from Moonlight". They play again once a game is in front.

Until you choose (the default), everything goes as before: the host's controller has PS and Create (every controller with
**PS for all players**), and every device gets "PS5 Screen". Choosing no profile closes the menus to all of them. Devices
that can choose freely keep the host's rule either way, as which profile the console seated their controllers in isn't
known. A change counts within a second, also during a stream. Devices shows "(menus)" after a device's profiles that
reach them.

**Set a sign-in passcode on your own profile.** The console's own user switching stays outside the host: a controller
that reaches the console's menus can still reach it.

## Keyboard and mouse as a controller

A device without a controller plays with its keyboard and mouse: they become a controller of their own, a player like any
other. If you already play with keyboard and mouse on a PC, it should feel familiar: W A S D move, the mouse turns the
camera, the mouse buttons are the triggers, Esc pauses. The controller appears with the first key or mouse button that is
one of its buttons; moving the mouse or turning the wheel alone doesn't add it, so someone who only watches takes no player
slot. Like any controller, it plays as the console's user if its device is the host (see above), and otherwise the
console asks "Who's using this controller?".

| Keys (every profile) | Controller |
|---|---|
| Enter | ✕ |
| Backspace | ○ |
| Esc | Options (pause) |
| Tab | touchpad (the map or inventory in most games) |
| F1 / F2 | PS / Create, the host only (as on every controller, see above) |

The rest depends on the profile, `keyboard_mouse_profile` in `sunshine.conf` (`standard` unless set). They follow the
keyboard layouts of PC games of each kind; [docs/keyboard-profiles.md](docs/keyboard-profiles.md) has every table and why.
All of it is optional: without settings, `standard` applies.

| Profile | For | What changes |
|---|---|---|
| `standard` | menus, most games | W A S D left stick, I J K L right stick, arrows D-pad, Space ✕, Q □, E △, Z / C L1 / R1, 1 / 3 L2 / R2, F / G L3 / R3, T touchpad; mouse: right stick, left button R2, right L2, middle R3, side buttons L1 / R1, wheel D-pad |
| `action` | third-person action, open worlds, souls-likes | Space ✕ (jump), Shift ○ (dodge, run), Q or R □, E △, Ctrl walks (half the stick); left mouse button R1, right L1, side buttons L2 / R2 |
| `shooter` | shooters (Call of Duty, Battlefield, Destiny 2, Apex, Far Cry, Cyberpunk) | Space ✕ (jump), Shift L3 (run), C or Ctrl ○ (crouch), E, F or R □ (interact, reload), V R3 (melee), Q L1, G R1; the wheel changes weapon (△) |
| `driving` | racing | W R2 (throttle), S L2 (brake), A / D steer, all three easing in over 0.2 s; Space ○ (handbrake), Shift △, E / Q ✕ / □ (gears), F L1 (look back), Ctrl halves throttle, brake and steering; the mouse doesn't steer |
| `fighting` | fighting games | W A S D and Space (up) on the D-pad, exact diagonals; U I O □ △ R1, J K L ✕ ○ R2, P L1, ; L2; no mouse |
| `platformer` | 2D platformers and metroidvanias | arrows move, Z ✕, X □, C R2, A ○, S L2, D △, E R1, Tab L1, I touchpad; no mouse |
| `sports` | football, basketball | W A S D left stick, I J K L or the numeric keypad right stick, Space ✕, F or Num 5 □, R △, E ○, Shift R2 (sprint), Ctrl L2, Q L1; no mouse |

**Vehicles:** in `standard`, `action` and `shooter`, Caps Lock switches a vehicle layer on and off, for games where you
walk and drive, and its light shows when it is on. Only W (throttle, R2), S (brake, L2), A and D (steering), Space
(handbrake, R1) and Ctrl (half throttle, brake and steering) change, easing in as in `driving`; every other key and the
mouse stay as on foot, so getting out, shooting from the car and the camera are where they were. The host can't see
whether you are in a car, so switch it yourself; if the light is the wrong way round, press Caps Lock twice. It suits GTA V
and the GTA Trilogy (Modern controls), Cyberpunk 2077, Far Cry 5 and 6, Watch Dogs, Days Gone, Death Stranding and Fortnite;
there the handbrake may sit on another button (○ in Far Cry 6 and Days Gone, L1 in Fortnite). Leave it off for horses
(Red Dead Redemption 2, Ghost of Tsushima, Horizon's mounts, Assassin's Creed): they ride like walking. Planes and
helicopters have no layer of their own yet.

A key gives full deflection, two keys on one stick a diagonal, and opposite keys cancel out, also on the D-pad. The mouse
turns the right stick faster the faster it moves and lets go shortly after it stops. **Ctrl+Alt and the wheel** set how
fast while you play, a step at a time (nothing shows on the console, which everyone sees); the host keeps it per device and
profile. Where Ctrl is a button (`shooter`, `sports`), press Alt before Ctrl, or Ctrl adds the controller like any button. It works in Moonlight's relative mouse mode (Ctrl+Alt+Shift+M), which games use.
Each wheel step is a short tap on the D-pad. Moonlight's own shortcuts (Ctrl+Alt+Shift and a key) stay Moonlight's.

**Menus:** the arrows, Enter, Backspace and Esc are the same in every profile. Hold **the key right of the space bar**
(right Alt or AltGr, the right ⌘ on a Mac) for the menu keys in any profile: `standard`'s, with W A S D on the D-pad,
which every menu takes; some, the console's own among them, ignore the stick. With no game running (none open, not even
suspended behind the home screen), the keys are the menu keys anyway.

**Typing:** the keyboard is a controller here, so it doesn't type into the console's text fields. Use the console's
on-screen keyboard (✕ on Enter picks a letter), connect a controller, or set `keyboard_mouse_pad = 0` to send the keys to the
console as before.

**By game:** with `keyboard_mouse_profile_by_game = 1` (off by default) the running game picks the profile, from a map
of about 3,000 PS4 and PS5 games made from Wikidata. `keyboard_profiles.txt` next to `sunshine.conf` is your own list,
which counts either way: a profile per title (any title ID, also homebrew and forwarders, such as your own Mario Kart 8
Deluxe forwarder set to `driving`) and profiles of your own with some keys changed. The host writes your installed games
into it as comments, so choosing one is removing a `#`. See [docs/keyboard-profiles.md](docs/keyboard-profiles.md).

**When a controller connects** to that device, it takes the keyboard's place quietly: the same player, so the console
doesn't ask again. From then on the keyboard and mouse are the console's keyboard and mouse, as before. If that
controller drops (a flat battery, a loose cable), the console says so; the next key on the keyboard takes a controller
again. `keyboard_mouse_pad = 0` in `sunshine.conf` turns all of this off. The settings aren't in the app yet; the host
reads them when it starts.

## Playing over the internet

A friend in another house can join like a device on the LAN. A contributor played that way with two of four players over the
internet (forwarded ports, a phone on 4G and one on another Wi-Fi). It isn't tested here. There are two ways in: forwarded
ports, below, or [Tailscale on the console](#without-port-forwarding-tailscale), which also works where you can't forward
ports.

1. **Pair the device at home first**, on the same network as the console, or pair over the internet once the ports are open;
   either way the PIN goes into the app on the console.
2. **Forward these ports** on your router to the console (the defaults; they move with **Base port** in the app's Settings):
   TCP 47984, 47989 and 48010, UDP 47998, 47999 and 48000. Nothing else needs to be open. Don't forward the
   [web page](#web-page) (TCP 47990), which answers only the LAN and Tailscale, or the control socket (TCP 47995), which only
   listens on the console itself anyway.
3. In Moonlight, **add the host by your public address** (or a dynamic DNS name for it).
4. **Lower the bitrate** in Moonlight to what the upload of your connection carries. Every player gets a copy of the stream,
   so two friends over the internet need twice the upload.

Tips:

- A friend whose Moonlight asks for **another resolution up to 1080p** (720p, say) or another codec gets a capture channel of
  their own, with their own bitrate. Asking for the same mode as you shares your stream, at your bitrate.
- **Encryption over the internet** in the app's Settings is on ("Optional") by default, so a client outside your
  network gets an encrypted stream when it supports it.

### Without port forwarding: Tailscale

Behind a double NAT, carrier-grade NAT or a router you can't change, forwarded ports don't help. [ps5-tailscale](https://github.com/holdmysocks/ps5-tailscale)
puts the console itself on your [Tailscale](https://tailscale.com) network as a payload, and from version 0.7.3 on it passes
ProsperoSunshine's ports by default. Nothing is open to the internet, only to the devices in your tailnet.

1. Install ps5-tailscale on the console as its README says, and log the console in to your tailnet.
2. Install Tailscale on the device you stream to, logged in to the same tailnet (or shared with it, for a friend).
3. In Moonlight, add the console by its **tailnet address** (the 100.x address or its name), pair, and stream.

If you set up ps5-tailscale before 0.7.3, its saved config keeps the old UDP list: add `47998, 47999, 48000` under its
**Settings**, or Moonlight pairs and lists the games but the stream fails with "control stream establishment failed".

Don't list the console itself as a host under ps5-tailscale's **Game streaming** (that's for Moonlight on the PS5 streaming
from a PC). ps5-tailscale then listens on the same ports on the console, the host can't open UDP 47999, and every stream
fails at the start, in Moonlight as "Starting RTSP handshake failed" (issue 24).

A contributor tested this on a PS5 Pro on 10.20 with Moonlight on iOS, and with a console in another home on 0.3.1; it isn't
tested here. Without a payload on the
console, a PC or router at home acting as a Tailscale subnet router works too; the friend then adds the console's LAN
address.

## Known limits

- On firmware 13.00, H.264 and HEVC stream at 1080p, 1440p or 4K, whichever the client asks for. On 13.00 and 10.20 a
  client asking for less than 1080p gets whichever of 720p, 540p and 360p is nearest to its request, so a weak client or Wi-Fi link has less to carry and decode: a PS Vita asking for 960x544
  gets 960x540, a Steam Deck (1280x800) 1280x720, and a 1600x900 screen still 1080p. These modes run at up to 30 Mbit/s (360p), 60 (540p) or 120 (720p), half of that at 30 fps,
  however much the client asks for, because 13.00 won't start them with more. Every other firmware streams 1080p whatever
  the request, and 10.20 does so for requests above 1080p, unless you turn on the
  [experimental switches](#testing-other-firmware). The frame rate follows the client: 30 or 60 fps.
- The console refuses 1440p and 4K capture in some titles (seen in a Mario 64 port and Teardown; GTA III works). The stream
  then runs at 1080p, also when such a title starts during a stream, and stays there until the next stream. The same should
  apply when the console's video output is smaller than the request; that is not tested yet.
- At 4K the console can show "Gameplay recording paused because you entered a blocked scene" shortly after the stream starts
  (seen in GTA III). The stream keeps running.
- HEVC and HDR are offered on firmware 13.00 and 10.20, elsewhere H.264 unless you turn on the
  [experimental switches](#testing-other-firmware). Clients that prefer HEVC pick it on their own. To force H.264, choose it in
  Moonlight's settings or set `hevc_mode = 1` in `sunshine.conf`, which also beats the switches; `hevc_mode = 2` keeps HEVC and
  turns HDR off.
- The bitrate is capped at 150 Mbit/s on firmware 13.00 and at 50 Mbit/s elsewhere, unless you turn on the
  [experimental switch](#testing-other-firmware). Above 50 Mbit/s it is measured only with a
  test client on wired LAN and a noise image at 4K, not yet with Moonlight or in a game. Games may stay well below what you set.
  With H.264, extreme images can exceed the setting: full-screen random noise reached about 140 Mbit/s at 4K and 35 Mbit/s at
  1080p, whatever was set.
- A second device can join a running stream as another player, with the same sound. Each device can get its own codec,
  dynamic range or resolution up to 1080p: the console runs a second capture channel next to the first for it. A device that
  asks for the same codec, dynamic range and resolution as a running stream shares that stream's picture, which costs the
  console nothing more. There are at most two channels: once both run, the host offers only their codecs and dynamic
  ranges, a further device joins the one with its codec and dynamic range and gets that channel's mode, and one whose
  Moonlight still asks for another dynamic range is refused with a message; one set to H.264 can't join while both run
  HEVC. A device asking for more than 1080p gets 1080p on the second channel. A stream that joined keeps the mode when the
  device holding the channel leaves. A contributor played with four players on 10.20. Each controller is new
  to the console, which asks "Who's using this controller?": pick a user, or Add User and then Quick Play for a guest. Every
  controller after the first is asked this way, also a second one on the same client, when it is first used. The console sends one copy of the
  stream per player, so a friend over the internet doubles its upload. While another device streams, quitting the app in
  Moonlight ends only your own stream; the last one to quit ends the app. A new stream from the same device replaces its
  old one. Measured here on 13.00 with the test client and a guest.
- Without `close_game_on_quit`, Moonlight for PC can still offer to quit a game from its app list, also when you pick another
  game, but that sends nothing to the host: it reports no running game once the stream has ended, so Moonlight skips its quit
  request and waits at "Quitting…". Close the game on the console instead. Other clients are untested.
- A real DualSense disconnects when the stream controller connects. Turned back on during the stream it stays without
  effect, because apps read only the controller that connected first; after the stream it works normally. The other way
  round, a controller that is already connected and not disconnected (seen with a second virtual pad) keeps the app, and
  only the console's home screen still reacts to Moonlight. If the stream controller does nothing in an app, turn the
  other controller off.
- Keyboard and mouse appear on the console the first time you use them in a stream ("Keyboard connected") and go when the
  stream ends. The console takes keys in its menus and in text fields once the on-screen keyboard is open. The mouse works
  only in games that support one (Planet Zoo does); the console's browser ignores it. Keys are sent by position as on a
  US keyboard, and the console's keyboard language setting decides what they type. Tested on 13.00 only.
- A device without a controller doesn't get the console's keyboard and mouse: its keyboard and mouse are a controller of
  its own ([Keyboard and mouse as a controller](#keyboard-and-mouse-as-a-controller)), also for the host, who then can't
  type into text fields with the keyboard; use the console's on-screen keyboard, connect a controller, or set
  `keyboard_mouse_pad = 0` in `sunshine.conf`. In Moonlight's absolute mouse mode the mouse stops at the edge of the
  window. Keys are remapped in `keyboard_profiles.txt` only, not yet in the app, and a game's own dead zone and top turning
  speed make the mouse feel unlike on a PC.
- Sound is stereo unless you turn on **Surround** under Experimental in the app's Settings. With it, a Moonlight that asks for
  5.1 or 7.1 gets the console's own multichannel sound. Which channel goes to which speaker is a guess so far (the order of the
  console's 8-channel audio output): the console used for testing outputs stereo, and then only front left and right carry
  sound. The console has to mix in 5.1 or 7.1 itself: connect it by HDMI to a receiver that reports 5.1 or 7.1 LPCM, and set
  Settings > Sound > Audio Output to HDMI Device Type "AV Amplifier", the number of channels and Linear PCM. With a TV that
  reports only stereo, the console mixes to stereo even with 5.1 selected (measured by a contributor on 13.60). If you have a 5.1 or 7.1 system, see [Testing other firmware](#testing-other-firmware) for how to help.
- No AV1, touchscreen or microphone.
- HDR has been seen on two real setups so far: Moonlight on an HDR TV (4K), and Moonlight on Windows with an HDR display and
  hardware decoding (1080p, since 0.2.4), one game each. The capture provides no HDR metadata, so the host sends the usual
  HDR10 values, and other screens may handle that differently. Screenshots that Windows takes of an HDR stream look washed out
  or burnt; that's the screenshot, not the stream.
- The pad's touchpad and motion sensors are measured with the test client only, not yet with a real Moonlight client or in a game
  that uses them. Motion has no orientation, because Moonlight sends none.
- Rumble is the classic two-motor kind only. Games that drive the DualSense's haptics send nothing to your controller.
  Whether your device vibrates also depends on the Moonlight client.
- Rest mode is young. The host starts its services again after waking, which worked in two tests on the home screen, with no
  game open and no stream running. With a game open and a stream running, the console and the game came back in one test, but the
  host stayed unreachable until it was restarted. In an earlier test without this, the console hung after waking and had to be
  switched off at the power button; the cause is unknown. To be safe, turn the console off instead.
- Adaptive triggers work with Moonlight on a PC and a DualSense; the other official clients ignore them. A user reports them
  working with R3VALATiON's Artemis fork on Android and a DualSense over Bluetooth. Only Teardown has been tested. If a stream
  ends during an effect, the trigger stays stiff until you connect again, because Moonlight doesn't reset it.
- Moonlight can't wake the console. It wakes a host with a Wake-on-LAN packet, and the PS5 ignores those: it only wakes for
  Remote Play's own wake message. The host can't help either, because the console stops it in rest mode. To wake the console
  from your phone, use a Remote Play client that can wake it, such as chiaki-ng, then start Moonlight.
- Starting or quitting a game briefly takes the capture resource away: video and audio pause for about 2 seconds
  and then resume on their own.
- After a hard network drop, held controller input is released after about 2 seconds.

## Troubleshooting

- **Moonlight cannot find the console:** Moonlight normally lists it on its own (mDNS, confirmed on iPhone). If not, for
  example because the network blocks multicast, add it by IP.
- **Connection fails or stalls:** make sure nothing blocks TCP 47984, 47989, 48010 and UDP 47998-48000 between the
  client and the console (client or router firewall).
- **iPhone:** works. An earlier bug (sound but no picture, because the host announced HEVC) is fixed.
- **Black or paused picture for a moment:** expected when a game starts or exits (see limits).
- **Controller works on the home screen but not in an app:** turn off other controllers connected to the console (see
  limits).
- **Other firmware:** 13.00 and 10.20 are tested. On other versions capture is refused if the system-library lookups fail;
  if they succeed it runs, untested. The kernel log says which case applies. See [Testing other firmware](#testing-other-firmware).
- **Diagnosis:** the app's **Diagnostics** page collects a report for a bug report, without addresses, device names, ids or
  game titles. While the page is open and the host runs, a phone or PC in the same network can open it at
  `http://<console-ip>:47996`; it is also saved as `/data/prosperosunshine/diagnostics.txt`. For more detail, set `min_log_level = debug` in
  `/data/prosperosunshine/sunshine.conf`; messages go to the kernel log and `sunshine.log`.

## Testing other firmware

HEVC, HDR, 1440p and 4K, and the modes below 1080p stay locked on firmware where nobody could check them. If you have such a
console, you can turn them on yourself and help find out. Nothing here has been seen working on an untested firmware yet, so
expect it to fail.

1. In the app, open **Settings**. Under **Experimental**, turn on what you want to try: HEVC (8 bit), HDR (10 bit), 1440p
   and 4K, a bitrate cap of 150 instead of 50 Mbit/s, or 720p, 540p and 360p. On firmware where a function is verified, its switch
   says it isn't needed. HDR also offers HEVC. The bitrate switch has no effect on firmware 10.x, because the encoder takes no
   bitrate there, and says so.
2. Choose **Save and restart host**.
3. Start a stream in Moonlight with that setting, for example with HEVC chosen as the codec, 4K or 720p as the resolution, or a
   bitrate above 50 Mbit/s. Check that the picture and sound are right.
4. Open the app's **Diagnostics** page and post the report in a [GitHub issue](https://github.com/Darkatek7/ProsperoSunshine/issues),
   whether it worked or not. Add your Moonlight client and what you saw on the screen. The report has no addresses, device names
   or ids in it.

**H.264 on firmware 10.x:** 10.x cuts every 1080p H.264 frame into 34 slices, and Moonlight on Windows then shows a white or
black picture with hardware decoding. The slice fields that 0.3.1 sets don't change that on 10.20 (measured by a contributor),
so use HEVC on 10.x, or software decoding for H.264. If you are on another 10.x version and stream H.264, the app's
**Diagnostics** page shows `first frame has N slices` (it is in `host_diag.log`, not in `sunshine.log`); the number helps.

**Surround:** turn on **Surround** under Experimental, set Moonlight to 5.1 or 7.1 and play a game with surround sound for a
minute. `sunshine.log` gets a line `12-channel levels` every 30 seconds; post a few of them with what you heard from which
speaker. That shows which channel is which.

**When it fails:** the host tries each function in more than one way. If every way fails, it turns that function off until it
restarts and offers H.264, 1080p and 50 Mbit/s again. A high bitrate falls back to 50 Mbit/s and a larger or smaller mode to 1080p within
the same stream; a smaller mode that already ran comes back with the next stream. With HEVC or HDR the
stream that failed ends with an error, because the client set itself up for that codec: start it again and it runs with what
the host still offers. If the client keeps asking for HEVC, choose H.264 in its settings. If the host itself goes down during a try,
start it again; it then keeps that function off for that run. To try again, restart the host, and to stop testing, turn the
switch off and choose **Save and restart host**.

## Development control socket

The host listens on TCP **47995** with a line-based control socket: `STATUS`, `PAIRINGS`, `PIN [<id>] <pin> [name]`,
`DEVICES`, `STREAMS`, `ENDSTREAM <uuid>`, `RENAME <uuid> <name>`, `UNPAIR <uuid>`, `REPORT`, `AUTOLOAD [ON|OFF]`, `RELISTEN`, `PROBE`,
`CANCELPIN <id>`, `WEBLOGIN [SET <user> <password>|ON|OFF|FORGET]` and `QUIT`. It is **unauthenticated**. By default it listens on `127.0.0.1` only (ROADMAP P9.1), so the app on the
console can use it but the LAN cannot. Creating the empty file `/data/prosperosunshine/control_lan` re-enables LAN
access for development. Do this only on trusted networks.

## Building from source

Requires Docker. The image contains LLVM 18 and the
[PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) v0.45 (hash-checked); all third-party sources are pinned
with SHA256 in `deps.lock`.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1
```

The first run builds the dependencies into `out/deps` (around 30 minutes, mostly Boost). The host is
`out/prospero_sunshine.elf`; the other ELFs are hardware probes. `app/build.ps1` then builds the app, and
`bash scripts/package.sh` the release archive. The host names its commit in `host_diag.log` and the diagnostics report.
`scripts/build.ps1` and CI pass it in. If you run `scripts/build.sh` in the container yourself from a git worktree, git can't find it
there and the host says `unknown`, unless you add `-e PROSPERO_COMMIT=<hash>`. Portable unit tests:

```sh
docker run --rm --mount type=bind,source="$PWD",target=/repo prosperosunshine-sdk:v0.45 bash scripts/test.sh
```

An automated Moonlight-protocol test client (pairing, smoke test, reconnects, packet loss, A/V sync) is in
`tools/moonlight/`.

## Credits

- **Special thanks to [EnriqueWood](https://github.com/EnriqueWood)**, who made firmware 10.x work and keeps testing on a
  PS5 Pro: the 10.x capture record, HEVC and HDR on 10.20, the modes below 1080p and how they reopen, the encoder's quality
  on 10.x, game audio that survives a capture reopen, the check before a game launch, and the option to close the game when
  you quit in Moonlight. He also played co-op with four players over the internet, tried several controllers on one client
  and HDR on Windows, came up with remote co-op and the PS button lock, and added ProsperoSunshine's ports to
  [ps5-tailscale](https://github.com/holdmysocks/ps5-tailscale). Thank you!
- Thanks to everyone who tested on their own console and firmware and reported back in the issues, and to
  [BuffMcBigHuge](https://github.com/BuffMcBigHuge) for the surround measurements on 13.60.
- The host is built on [Sunshine](https://github.com/LizardByte/Sunshine) and
  [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c), with the
  [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk).
- The capture record layout for firmware 13.60 comes from [PlayBridge](https://github.com/ramalhoboss/PlayBridge) and
  [ps5-stream8](https://github.com/lescanotomas/ps5-stream8). According to [seregonwar](https://github.com/seregonwar),
  both build on the analysis of seregonwar's PBS (PlayStation Broadcast System) payload. The record size for 12.40 comes
  from a log in the [PBS issue tracker](https://github.com/seregonwar/PBS-Public-Issues-Tracker).

## License

GPL-3.0-only, like Sunshine, which this project builds on (`LICENSE`). Third-party components, their licences and
the patches applied to them are listed in `docs/licenses.md`; what a release must bundle is in `docs/THIRD_PARTY.md`.
The build installs third-party licence texts to `out/deps/licenses`.
