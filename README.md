<p align="center"><img src="art/firebrand-github-social-1280x640.png" alt="Firebrand: graphics for PowerPC" width="640"></p>

# Firebrand

Firebrand is a graphics driver for AMD Radeon cards on Mac OS X 10.5 Leopard on PowerPC.
It brings three generations of GCN cards to Leopard: Polaris (GCN 4), Vega 20 (GCN 5) and
Southern Islands (GCN 1); [Status](#status) says what works on each. On Polaris: the desktop
at up to 3840x2160 at 60 Hz over DisplayPort, Quartz Extreme and Core Image on the GPU,
OpenGL 3.3 through Mesa's radeonsi driver, hardware H.264 decoding, DisplayPort audio and
power management.

The kernel driver is written after Linux's **amdgpu**: one file per hardware block
(`gmc_v8_0`, `gfx_v8_0`, `sdma_v3_0`, `uvd_v6_0`, `dce_v11_0`, the SMU7 powerplay code), with
amdgpu's function names, so code ports from Linux file for file. Every structure the GPU reads
is written little-endian explicitly, which is what a big-endian host needs and what amdgpu
never had to do. The userland (the OpenGL bundle, the window server's 2D plug-in and the video
library) comes from [RadeonNI-OSX](https://github.com/kouta-kun/RadeonNI-OSX), whose interface
Firebrand's accelerator speaks, with Mesa's radeonsi made to run on a big-endian host.

> **Status: early.** Firebrand runs today on Mac OS X 10.5.8 on NXP QorIQ T4240 (e6500)
> systems, where everything below was measured. **Power Mac G5:** Firebrand and HDAudio now
> take every device address through IOKit (`IODMACommand`), which on a G5 goes through its DART
> IOMMU, so the same build is meant for both machines. It has not been run on a G5 yet; see the
> [roadmap](#roadmap). There is no installer package or binary release yet; see
> [Building](#building).

## Status

What has been verified on hardware, by card family:

| Family | Verified card | Works | Known issues, not yet |
|---|---|---|---|
| **Polaris** (GCN 4) | RX 550, E9173 | Everything under [Features](#features): the display over DisplayPort, 2D, Quartz Extreme, Core Image, OpenGL 3.3, H.264 decoding (UVD 6.3), power management, DisplayPort audio, recovery | HDMI and DVI not tried on a monitor yet. The current OpenGL bundle (its Core Image and index-buffer fixes) has not been re-run on Polaris yet. |
| **Vega 20** (GCN 5) | Instinct MI50 | The display over DisplayPort, Quartz Extreme, Core Image, OpenGL 3.3, DisplayPort audio, H.264 decoding (UVD 7), power management and clock gating | The DMA engine occasionally stalls on its first frame after boot; the driver detects that and recovers it automatically. (Fixed in this version: draws that followed a pixel readback could go missing, which made Doom 3 flash geometry.) |
| **Southern Islands** (GCN 1) | Radeon R7 450 | The display over DisplayPort, 2D, Quartz Extreme, Core Image, OpenGL 3.3 | Power management is off: the card runs at its boot clocks. Video decoding (UVD 3.1) and DisplayPort audio are ported but not yet tested. Acceleration was verified with `FirebrandBringUp` on: the card's row in `amdgpu/frb_drv.c` still stops at the display stage, so a default install drives the display only. |

Other cards in these families do not load yet: the driver matches only the device IDs below.
The plan is for them to load display-only (stage `dce`) until each one is verified.

## Supported cards

| Card | GPU | PCI IDs | Status |
|---|---|---|---|
| AMD Radeon RX 550 (4 GB, DP + HDMI + DVI) | Polaris 12 | `1002:699f` rev `c7`, subsystem `1002:0b04` | verified |
| AMD Embedded Radeon E9173 (2 GB, 3x DP) | Polaris 12 | `1002:699f` rev `81`, subsystem `1002:ea73` | verified |
| AMD Instinct MI50 (16 GB HBM2, mini-DP, Radeon Pro VII firmware) | Vega 20 | `1002:66a1` rev `06`, subsystem `1002:081e` | verified |
| AMD Radeon R7 450 (Dell, 4 GB, DP + DP + DVI) | Cape Verde (Southern Islands) | `1002:682b` rev `87`, subsystem `1028:1004` | verified (see [Status](#status)) |
| AMD Radeon RX 570 | Polaris 10 | `1002:67df` | expected next; not enabled yet |

Firebrand only drives cards it has been verified on. Each one has a row in
`amdgpu/frb_drv.c` (device, revision and subsystem IDs), and the row caps how far the
bring-up may go. Another `1002:699f` card, such as another maker's RX 550, is identified and
then left alone; supporting it means adding its row once it has been checked. With
Firebrand installed, the kernel log shows such a card's IDs (`dmesg | grep Firebrand`).
Please open an issue with them.

## Features

On Polaris; [Status](#status) has the other families.

- **Display:** DisplayPort from 1.62 to 5.4 Gbit/s per lane (RBR to HBR2), up to 3840x2160 at
  60 Hz, with modes from the monitor's EDID. A monitor can be plugged into any port at any
  time (hot-plug). Hardware cursor, display sleep, 32, 16 and 8 bits per pixel. With no monitor
  the framebuffer still runs (headless). There is a boot screen with the Firebrand logo.
  HDMI and DVI are implemented but have not been tried on a monitor yet.
- **Quartz Extreme and Core Image** on the GPU.
- **OpenGL** through Mesa's radeonsi driver (`RadeonNIGLDriver.bundle`), for 32- and 64-bit
  programs: OpenGL 2.1 for ordinary programs (Leopard's OpenGL framework stops at 2.1), and
  OpenGL 3.3 core profile for programs that choose its renderer (ID `0x00021a01`).
- **2D acceleration:** the window server's fills, copies and region moves run on the SDMA
  engines (`RadeonNIGA.plugin`).
- **H.264 decoding** on the card's UVD 6.3 engine, Baseline to High profile up to 1080p60,
  bit-exact against FFmpeg on 12 test streams: about 60-77 pictures/s at 1080p and 140-150
  at 720p on an RX 550. Programs use it through RadeonNI's `'rdnv'` client library
  (`radeonni/video`); the FrostByte browser does.
- **Power management:** amdgpu's powerplay with the SMU7. Clocks follow the load (RX 550:
  214-1183 MHz engine clock, up to 1500 MHz memory clock), the memory clock switches under a
  live display without a visible glitch, and the fan and thermal control are the SMU's.
- **DisplayPort audio** to the monitor, with the `HDAudio` kext.
- **Recovery:** a graphics or SDMA ring that stops for 2 seconds is reset by a watchdog (the
  reset itself takes 28-57 ms), and reloading the driver resets the GPU.

Not yet: HDMI audio, video encoding (VCE), interrupts (the driver polls), compute queues, and
unloading the driver after the window server has opened it (restart instead).

## Requirements

- Mac OS X **10.5.8** on PowerPC, with a PCI Express slot. See the status note above for which
  machines that currently means.
- A supported card and a DisplayPort monitor.
- To build: a Linux or macOS machine for the OpenGL bundle and the firmware, and Xcode 3.1
  on the Mac for the kexts (see [Building](#building)).

## Installing

Firebrand is installed from the folder the build produces, `build/dist/Firebrand`. Read all
of this before starting: a card without a driver shows nothing at all, so you need a way back.

**Before you start**

1. Turn on **Remote Login** (System Preferences > Sharing) and check that you can log in
   with `ssh` from another computer. Note the Mac's address.
2. Leave the **old graphics card** in the Mac for the installation.

**Install**

```sh
cd Firebrand          # the build/dist/Firebrand folder, copied to the Mac
sudo ./install.sh
```

This installs into `/System/Library/Extensions`:

| Item | What it is |
|---|---|
| `Firebrand.kext` | the driver, set up as the framebuffer with 2D, OpenGL, video, power management and audio |
| `RadeonNIGLDriver.bundle` | the OpenGL driver (`--no-gl` leaves it out) |
| `RadeonNIGA.plugin` | the window server's 2D plug-in |
| `HDAudio.kext` | DisplayPort audio (`--no-audio` leaves it out) |

and `frbctl` into `/usr/local/bin`. Each kext is checked with `kextload -nt` (which loads
nothing) before anything is replaced. An OpenGL bundle or 2D plug-in from another driver
(RadeonNI uses the same names) is saved first and put back by the uninstaller.

Then shut down, put the AMD card in, connect a **DisplayPort** monitor and start the Mac.
There is **no picture until the driver starts**: PC graphics cards have no Open Firmware
driver, so the grey boot screen does not appear. Once Firebrand starts, its boot screen
appears, then the login window.

To check: `frbctl info` (the card, the bring-up stage, video memory) and System Profiler >
Graphics/Displays ("Quartz Extreme: Supported", "Core Image: Hardware Accelerated").

**If the screen stays dark**

- Log in with ssh and run `sudo ./uninstall.sh` in the same folder, then `sudo reboot`; or
- log in with ssh and run `sudo nvram boot-args="frb_enable=0"`, then restart: the driver
  stays installed but does not start (`sudo nvram -d boot-args` undoes this); or
- put the old card back and start in **Safe Mode** (hold Shift while the Mac starts; no
  third-party drivers load), then run `sudo ./uninstall.sh`; or, with the old card back,
  start in **single-user mode** (hold Command-S; Firebrand does not load there) and run
  `mount -uw /`, `rm -rf /System/Library/Extensions/Firebrand.kext`,
  `touch /System/Library/Extensions`, `reboot`.

## Firebrand Monitor

`Firebrand Monitor.app`, in the same `build/dist/Firebrand` folder (copy it to Applications),
shows what the card is doing:

- **In the menu bar:** the card's temperature, GPU clock and GPU load, such as
  "48° 1.7 GHz 85%". Its menu lists each card's clocks, loads, temperatures and power.
- **In its window:** a minute of history for the GPU clock and load, the memory clock,
  the temperature and the power, one page per card, and under the graphs the driver's state:
  the bring-up stage, power management, the displays and their DisplayPort links, the video
  decoder, the PCIe link and video memory in use.
- **Copy Report** puts all of it on the clipboard, ready to paste into a GitHub issue. Please
  add it to any problem you report. `frbctl report --md` prints the same in Terminal.

It needs no administrator password: it only reads, through a read-only connection to the
driver that any user may open. Every control stays in `frbctl`. To start it at login, add it
under System Preferences > Accounts > Login Items: started that way it shows no window, only
its menu-bar item and Dock icon; click the Dock icon, or choose "Open Firebrand Monitor…" from
its menu, for the window.

The readings depend on the card: all of them on Vega 20 and Polaris. On the R7 450, with power
management off, there are no clock, load or power readings, only the temperature and the
driver's state; that card has not been run with the monitor yet.

## Uninstalling

```sh
sudo ./uninstall.sh
```

removes what `install.sh` installed and restores the saved OpenGL bundle and 2D plug-in.
Restart afterwards.

## Building

The pieces are built in three places:

| Piece | Built on | With |
|---|---|---|
| AMD microcode (`firmware/`) | a Linux or macOS machine | `curl`, `sha256sum` or `shasum`, `zstd` or `xz` |
| `RadeonNIGLDriver.bundle` (Mesa) | a Linux or macOS machine | a `powerpc-apple-darwin9` GCC 15.3 cross toolchain and the Mac OS X 10.5 SDK |
| `Firebrand.kext`, `HDAudio.kext`, `RadeonNIGA.plugin`, `frbctl`, `Firebrand Monitor.app` | the Mac: Mac OS X 10.5.8 | Xcode 3.1 (Apple gcc 4.0.1, the 10.5 SDK) |

The kexts need Apple's gcc 4.0.1: the kernel's C++ ABI is that compiler's. Mesa needs a
current GCC. The microcode download needs a current TLS stack, which 10.5 lacks.

**The toolchain and the SDK.** The cross toolchain is the one the
[FrostByte](https://github.com/jbettcher-wg/frostbyte) browser uses. In a FrostByte checkout,
`./build.sh sdk toolchain --sdk=/path/to/xcode314_2809_developerdvd.dmg` downloads a
prebuilt toolchain (or builds it) and imports the SDK from your copy of Xcode 3.1.4. Apple's
licence does not allow the SDK to be redistributed, so you supply it.

**From a clone:**

```sh
git clone https://github.com/jbettcher-wg/firebrand
cd firebrand
./build.sh firmware                           # the AMD microcode, checksum-verified
FROSTBYTE=~/frostbyte ./build.sh gl fat       # the OpenGL bundle, 32- and 64-bit
./build.sh mac user@your-mac                  # the rest, built on the Mac over ssh
```

`./build.sh mac` copies the tree (with `firmware/` and the OpenGL bundle) to the Mac, runs
`scripts/build-mac.sh` there and brings back `build/dist/Firebrand`. On the Mac itself,
`./build.sh native` does the same once `firmware/` and `build/gl/` are there.
The kext includes the Instinct MI50 and its microcode by default (`FRB_BRINGUP=vega20`);
`FRB_BRINGUP=` builds it for the Polaris and Southern Islands cards alone.

Leopard's sshd (OpenSSH 4.5) only speaks algorithms a current OpenSSH client has turned off,
so enable them for the Mac in `~/.ssh/config`:

```
Host your-mac
    HostKeyAlgorithms +ssh-rsa
    PubkeyAcceptedAlgorithms +ssh-rsa
    KexAlgorithms +diffie-hellman-group14-sha1
    Ciphers +aes128-cbc
    MACs +hmac-sha1
```

`LEOPARD_TOOLCHAIN` and `LEOPARD_SDK` can name a toolchain and SDK directly, instead of
`FROSTBYTE`. `radeonni/build-gl.sh` downloads Mesa 26.2.4 and puts meson and mako in a private
Python environment under `build/`; it checks the bundle it makes (all 66 entry points Leopard's
GL engine needs, no unresolved symbols, system libraries only), because the GL engine falls
back to software rendering without a word when it rejects a bundle.

`scripts/build-mac.sh` checks every kext it builds for 24-bit branches. A kext is loaded about
2.3 GB away from the kernel's code, so every call must be a long branch (`-mlong-branch`).

## How it works

**Bring-up stages.** The driver brings a card up in the order amdgpu does, in stages that can
be stopped at:

| Stage | What runs |
|---|---|
| 0 survey | identity, VBIOS (from the card's ROM), AtomBIOS command tables; no register writes |
| 1 post | AtomBIOS `ASIC_Init` when the card is not posted |
| 2 gmc | memory controller, its microcode, VRAM and GART |
| 3 smu | the SMU and its firmware |
| 4 dce | the display engine: connectors, EDID, link training, modes |
| 5 gfx | the graphics engine (RLC, CP), ring and IB tests |
| 6 sdma | both SDMA engines; after this, power management (DPM) |
| 7 uvd | the video decoder |

`install.sh` sets the installed driver to stage 7 with everything on. A card's row in
`amdgpu/frb_drv.c` caps the stage, and a card without a row only gets the survey.

**Settings.** The installed driver reads its settings from its personalities in
`Firebrand.kext/Contents/Info.plist` (restart after editing):

| Key | After install | |
|---|---|---|
| `FirebrandStage` | 7 | how far the bring-up goes |
| `FirebrandAccel` | on | the accelerator: 2D, OpenGL and video |
| `FirebrandAccelGL` | on with the GL bundle | offer the accelerator to OpenGL (Quartz Extreme, Core Image) |
| `FirebrandDPM` | on | power management; off keeps the boot clocks |
| `FirebrandAudio` | on | DisplayPort audio |
| `FirebrandHWCursor` | on | the hardware cursor |
| `FirebrandVideo` | on | H.264 decoding |
| `FirebrandBootPattern` | `auto` | the picture before the login window: `splash`, `bars`, `grey` or `black` |

Boot arguments (`sudo nvram boot-args="..."`) turn parts off without editing anything:
`frb_enable=0` (the driver does not start), `frb_stage=4` (the display only: no acceleration,
OpenGL, video or power management), `frb_hwcursor=0`, `frb_video=0`, `frb_hotplug=0`.

**frbctl** talks to the running driver (as an administrator): `frbctl info`, `log`, `display`,
`detect`, `modes N`, `dpm [status|auto|high|low]`, `audio [status]`, `crc N` (the monitor's
checksum of what it receives), `test gfx|sdma0|sdma1|uvd ...` (engine tests), `recover`, and
register-level commands for bring-up work. The comment at the top of `tools/frbctl.c` lists
them all. `frbctl report` needs no administrator: it prints what
[Firebrand Monitor](#firebrand-monitor)'s Copy Report copies.

**Byte order.** The GPU is little-endian and the PowerPC is big-endian. Firebrand writes
every structure the GPU reads (rings, page tables, command buffers, the SMU's tables, the UVD
messages) in the GPU's byte order explicitly, instead of using the hardware byte swappers,
which do not cover all of them. The Mesa patches do the same for radeonsi.

## Source layout

| Path | What |
|---|---|
| `amdgpu/` | the hardware library, ported from Linux amdgpu file for file (`frb_gmc_v8_0.c` is `gmc_v8_0.c`, and so on); `include/` has AMD's register headers, unchanged |
| `shared/` | the OS layer (register access, memory, firmware, logging), the AtomBIOS interpreter, EDID and mode tables, the boot screen and its logo |
| `kext/Firebrand/` | the IOKit side: the card's owner, the framebuffer, the accelerator and its user clients, the surfaces, video; `Info.plist`, `Makefile` |
| `kexts/HDAudio/` | the HD Audio driver for the card's DisplayPort audio |
| `radeonni/` | the userland: the OpenGL bundle and the Mesa patches, the 2D plug-in, the video library ([radeonni/README.md](radeonni/README.md)) |
| `tools/` | `frbctl`; `frb_status.c`, the status reading `frbctl report` and Firebrand Monitor share; `FirebrandMonitor/`, the app; `mk-splash-logo.py`, which makes the boot logo header from `art/C-chip.svg` |
| `scripts/` | `fetch-firmware.sh`, `build-mac.sh`, `build-remote.sh`, `install.sh`, `uninstall.sh` |
| `art/` | the logo |

The boot logo header (`shared/frb_splash_logo.h`) is committed, so a build needs no image
tools. To change the logo, edit `art/C-chip.svg` and run `tools/mk-splash-logo.py
art/C-chip.svg`; it needs librsvg (`rsvg-convert`), ImageMagick (`magick`) and the Noto Sans
font installed.

## Roadmap

- **Installer:** a flat `.pkg` with checks before it installs anything, an uninstaller, and
  RadeonNI handled where it is installed.
- **Power Mac G5:** the DMA side is done (addresses through IOKit, so the DART translates them);
  next is a test run on G5 hardware, then an Open Firmware boot path so the card shows the boot
  screen from power-on.
- **Vega 20:** running on the Instinct MI50 with the PSP, GMC 9, GFX 9, SDMA 4, SMU 11,
  UVD 7 and DCE 12 blocks (see [Status](#status)). Next: other Vega 20 cards (Radeon VII,
  Radeon Pro VII).
- **Southern and Sea Islands:** the R7 450 runs as amdgpu's SI blocks (see [Status](#status));
  next, its power management, video decoding and DisplayPort audio, then Sea Islands (CIK, for
  example the FirePro W8100) from RadeonNI, so one driver covers GCN 1 to 5.
- **Cards without their own row:** load display-only until verified, with a command to turn
  the rest on and a report to send back.
- **More Polaris cards:** the RX 570 (Polaris 10) and other makers' boards.
- **Vulkan:** Mesa's RADV on a new Firebrand kernel interface (per-program GPU address spaces,
  command submission with buffer lists, sync objects), with a Vulkan loader and window-system
  integration for Leopard; then Zink, OpenGL 4.6 over Vulkan.

## The name

Radeon red is the fire. When AMD launched Vega it called the campaign the "Radeon Rebellion", and a firebrand is a rebel who stirs things up. A driver that brings modern Radeons to Mac OS X on PowerPC, long after anyone expected one, is a bit of both.

## Licence

Firebrand's own code is under the [MIT licence](LICENSE). The code ported from Linux and AMD's
headers is MIT-licensed with its original copyright notices kept in each file; RadeonNI-OSX's
code is MIT ([radeonni/LICENSE](radeonni/LICENSE)); the Mesa patches are under the licence of
the Mesa files they change (MIT). AMD's GPU microcode is not in this repository: it is
downloaded at build time and redistributable in binary form under AMD's licence
([LICENSES/LICENSE.amdgpu](LICENSES/LICENSE.amdgpu)). The wordmark is set in Noto Sans (SIL
Open Font License, [art/OFL-NotoSans.txt](art/OFL-NotoSans.txt)). [NOTICE](NOTICE) lists every
component.

Firebrand is not affiliated with or endorsed by AMD or Apple. AMD, Radeon and Polaris are
trademarks of Advanced Micro Devices, Inc.; Mac OS X and Quartz Extreme are trademarks of
Apple Inc.

## Acknowledgements

Firebrand builds on the Linux amdgpu and radeon drivers and their authors at AMD and in the
community, on [Mesa](https://mesa3d.org), and on kouta-kun's
[RadeonNI-OSX](https://github.com/kouta-kun/RadeonNI-OSX), which first ran a Radeon's 3D engine
under Mac OS X on PowerPC.
