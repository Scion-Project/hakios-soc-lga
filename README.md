# Google Tensor G5 ("laguna", `lga`): HakiOS's SoC layer

HakiOS (Scion's XNU kernel) on the Google Tensor G5, the SoC of the Pixel 10 (`frankel`), 10 Pro (`blazer`), 10 Pro XL
(`mustang`) and 10 Pro Fold (`rango`). This is the layer they share: the SoC's facts (`soc.conf`, `configs/lga.config`) and
the **boot shim** that starts HakiOS on a Tensor G5 without a UEFI (`shim/`). The phone's own repository is
[Scion-Project/hakios-frankel](https://github.com/Scion-Project/hakios-frankel); both are checked out inside the kernel's
common repository, [Scion-Project/hackios-xnu-kernel](https://github.com/Scion-Project/hackios-xnu-kernel), by
`tools/hakios-device.py sync` (`socs/lga`, `devices/frankel`), as `devices/manifest.conf` names them.

> **Nothing here has run on a phone.** The shim, the loader's platform and the kernel's half are checked in QEMU (below)
> and on the host; on a Pixel 10 they are untested. Running them needs an **unlocked boot loader, and unlocking a Pixel
> wipes it** (`fastboot flashing unlock` erases all user data). Use a second phone, or decide on the wipe knowingly:
> [hakios-frankel's README](https://github.com/Scion-Project/hakios-frankel#before-anything-the-phones-data) says what it
> takes.

| Path | What |
|---|---|
| `soc.conf` | the SoC, for `tools/hakios-device.py` (docs/porting/repositories.md) |
| `configs/lga.config` | the SoC's kernel fragment: `CONFIG_SOC="lga"`, one core for the first boot, none of the Qualcomm SoC drivers |
| `shim/` | the boot shim: an arm64 Linux Image that offers xnuloader the UEFI services it uses, over the boot loader's device tree |
| `dts/mainline/` | the Tensor G5's device trees from mainline Linux 7.4 (BSD-3-Clause), the reference for the facts below |

## The hardware

Every fact has a source: **M** is mainline Linux, `arch/arm64/boot/dts/google/` at
[`18686b294c6f`](https://github.com/torvalds/linux/commit/18686b294c6f6ba778cba66eccf01b9fcdf48988) (copied in `dts/mainline/`);
**G** is Google's kernel for the Pixel 10 family, [azagramac/pixel10-kernel-aosp](https://github.com/azagramac/pixel10-kernel-aosp)
at `6f689be7fa23` (GPL-2.0: **read for facts, nothing copied**), paths below `private/`. Where the two agree, M is cited first.

| What | Value | Source |
|---|---|---|
| SoC | Google Tensor G5, "laguna", `google,lga`; B0 silicon in the phones sold (product id 5, A0 on EVT units) | M `lga.dtsi` header, `lga-frankel.dts`; G `devices/google/lga/dts/lga-b0.dts` |
| Cores | 8, each in an affinity field of its own (MPIDR Aff1 = 0-7): `cpu@0`, `@100` Cortex-A520 ("hayes", capacity 258); `@200`-`@600` Cortex-A725 ("hunter", 891); `@700` Cortex-X4 ("hunterelp", 1024). One `cpu-map` cluster | M `lga.dtsi` `cpus` |
| PSCI | 1.0, by SMC; per-core and cluster idle states (`arm,psci-suspend-param` 0x40000003, 0x40010033, 0x40020333) | M `lga.dtsi` `psci`, `idle-states` |
| Interrupts | GICv3 (`arm,gic-v3`): distributor 0x05880000 (64 KB), redistributors 0x05900000 (2 MB: 256 KB a core, the GICv4 layout with VLPI frames); maintenance PPI 9; 4 interrupt cells, PPI partitions (the X4 apart). Most peripherals' interrupts go through Google's interrupt aggregators ("GIA") | M `lga.dtsi` `interrupt-controller@5880000`; G `lga/dts/lga-base.dtsi`, `lga-dpu.dtsi` |
| Timer | the architected timer: PPIs 13, 14, 11 (virtual), 10 (EL2); no `clock-frequency`, so CNTFRQ_EL0 is the firmware's. **Inferred** 38.4 MHz: the GTC's clock (`gtc_osc`) is 38.4 MHz and Google's GTC driver says the arch timer runs at the GTC's rate. The loader reads CNTFRQ_EL0 itself | M `lga.dtsi` `timer`; G `lga-base.dtsi` `oscillator_38m`, `sst@200c1000`; `google-modules/soc/rdo/drivers/soc/google/google_gtc.c` |
| Oscillator | 38.4 MHz | M `lga-pixel-common.dtsi` `&osc` |
| UART | a DesignWare APB UART (`google,lga-uart`, `snps,dw-apb-uart`; 16550-style, reg-shift 2, 32-bit), 0x0DB62000, 200 MHz, SPI 688. `disabled` in the tree: the boot loader enables it when its console is on. Android's console: `earlycon=uart8250,mmio32,0xdb62000`, 115200 8N1. Not a PL011: xnuloader's UART is not it | M `lga.dtsi` `serial@db62000`, commit message of `54edd8698bb1`; G `lga-lsio.dtsi`, `lga-chosen.dtsi` |
| Watchdogs | two Google watchdogs, `google,wdt`: 0x200C4000 (24 s, a FIQ to EL3) and 0x200C5000 (30 s, the GDMC restarts the SoC). ID register 0x00 reads 0x574454 ("WDT"); control 0x08 (bit 0 enable); a write is taken after the key 0x00C0FFEE at 0x10, locked again with 0 | G `lga-base.dtsi` `watchdog_cl0/cl1`; `google-modules/soc/rdo/watchdog/google_wdt.c`, `google_wdt_local.h` |
| Display controller | VeriSilicon DC9x00 at 0x0EE00000 (`sswrp_dpu` 0x0EC00000 + 0x200000, 1.1 MB); it reads through an SMMUv3 at 0x0ED80000. Layer 0's shadow registers: address +0x1802C (high byte +0x18030), stride +0x18044, size +0x1805C (width 15:0, height 31:16), config +0x18088 (enable bit 0, format 29:24, 0 = A8R8G8B8); the next layer 0x4000 on; output 0's start +0xB6000. Synopsys DSI2 host 0x0EFD8000, C/D-PHY 0x0EFC0000 | G `lga-dpu.dtsi`, `lga-smmu.dtsi`; `google-modules/display/vsi-drm/verisilicon/9x00/vs_dc_reg_fe0.h`, `vs_dc_hw.c` (`plane_set_fb`), `vs_dc_reg_be.h` |
| Boot framebuffer | **not in any tree** (no `simple-framebuffer`, no splash region): wherever the boot loader put it. The shim finds it (below) | M, G: absent |
| UFS | Google's host, `google,ufshc`: 0x3C400000 (HCI), 0x3C410000 (top), 0x3C700000 (PHY SRAM); behind an SMMUv3 (`inf_smmu`, identity-mapped); the boot device (`androidboot.boot_devices=3c400000.ufs`). Its rails come from the PMIC through the CPM's mailbox. The boot loader demands a `ufs0` node in its tree to add calibration to | G `lga-base.dtsi` `ufs@3c400000`, `lga-chosen.dtsi`; LKML, v1 of the mainline series |
| PMIC | Dialog/Renesas DA9188 (main) and DA9189 (sub) "AP-PMICs", driven through the CPM (Google's power manager) by mailbox (`mba-dest-channel`), not by the AP's own bus | G `lga-ap-pmic.dtsi` |
| GPU | Imagination PowerVR DXT-48-1536 | G `google-modules/powervr`; Phoronix |
| RAM | LPDDR5X from 0x80000000; the boot loader writes the `/memory` banks. The 4-8 GB physical range is unassigned | G `lga-base.dtsi` `memory@80000000`, `lga-rmem.dtsi` (`pkvm_smmu_v3_prot_reg@100000000`) |
| ramoops | 0x95200000, 4 MB: console 2 MB, then pmsg 2 MB (0x95400000); not `no-map`. Reserved "for access by the ABL following a crash-reset": a warm restart keeps it | M `lga.dtsi` `ramoops@95200000`; G `lga-rmem.dtsi` |
| Other reservations | boot loader logs 0x95600000 (1 MB); ABL 0xBE000000 (16 MB); GSA log 0xA61B0000 (16 KB in M, 0xA61B1000 4 KB in G); and in G: AOC 0x8D200000 (48 MB), TPU 0x9FC00000 (20 MB), GSA 0xA2400000 (62 MB), GPU firmware 0xB9F00000 (33 MB), `s2m` 0xC0000000 (74 MB), modem 0xE8000000 / 0xEA400000, 0xF6200000, and some 20 more, many `no-map` | M `lga.dtsi` `reserved-memory`; G `lga-rmem.dtsi` |

The boot loader (ABL) changes the tree it hands a kernel: it fills `/memory` in, enables the UART for its console, adds
UFS calibration, and **stops with an error if a node it wants to change is missing** (mainline's series had to wait for a
new boot loader for that reason). The shim never gives it a tree: the boot image carries none, so ABL uses the phone's own
from `vendor_boot`/`dtbo`, and the shim reads what ABL made of it.

## How HakiOS starts here

On the Galaxy S9 the boot loader starts a UEFI (Project Silicium's), the UEFI starts `xnuloader`. **There is no UEFI for
a Tensor phone**: Project Silicium has no Google platform or silicon package (its `Silicon/` has Qualcomm, Samsung, MediaTek,
Nvidia), and nobody else has published one. Two ways were weighed:

| | A UEFI port (Mu-Silicium) | A shim (chosen) |
|---|---|---|
| What it is | a `GooglePkg`/`LagunaPkg` silicon package and a `frankelPkg` platform: memory map, GIC, timer, a framebuffer GOP, PSCI, and to be of use, UFS and USB drivers Silicium does not have for this SoC | `shim/`: about 2000 lines of C and assembly, the services `xnuloader` uses and nothing more |
| UFS | would have to be written for UEFI first (Silicium's are Qualcomm's and Samsung's) | written once, in the kernel (the next step either way) |
| How much is new | an EDK2 port, its build, its binaries, kept in step with Silicium | small, all of it ours, MIT |
| Boot loader handover | the same problem (EL2, framebuffer, watchdogs) solved in EDK2 | solved in a few hundred lines |

**The shim it is.** It is what PixOS did on the Pixel 6a (below), with one difference: their shim boots their own XNU
directly; this one starts HakiOS's own `xnuloader`, unchanged, so everything the loader does on the S9 and the POCO (the
payload, the kernel's window, its device tree, a run on another core, the return and the log) is the same here.

```
fastboot boot xnu-frankel.img            (RAM only: nothing is written to the phone)
 └ ABL (Google's, unlocked)  ── boot image v4 + AVB footer; the device tree is ABL's own, from vendor_boot/dtbo
    └ shim (socs/lga/shim, EL2 -> EL1)  ── an arm64 Image; x0 = ABL's device tree
       │   EL2 kept: PSCI by HVC passed on to EL3 by SMC; CPU_ON starts a core at EL1
       │   EL1: the RAM map from the tree, MMU on, watchdogs stopped, the framebuffer found,
       │        the payload copied to 0xF0000000, loader.efi loaded with a UEFI system table
       └ xnuloader (platform "frankel", found by the tree's root compatible "google,lga-frankel")
          └ XNU (EL1)  ── GICv3/v4, the timer, the boot loader's framebuffer; root in RAM
```

### The shim (`shim/`)

| File | What |
|---|---|
| `start.S` | the arm64 Image header; self-relocation; EL2 set up (HCR_EL2.RW, API/APK where there is pointer authentication; the timer and the GIC's system registers open to EL1; FP/SVE, debug, fine-grained traps and HCRX cleared) and the drop to EL1; EL2's vectors: HVC is passed on to EL3 by SMC, CPU_ON rewritten so the firmware starts the core at `secondary_el2`, which enters the caller's entry point at EL1 |
| `shim.c` | at EL1: the tree read, a 1:1 map (RAM cacheable in 2 MB blocks, `no-map` regions and everything else as devices', the framebuffer normal non-cacheable), the UEFI system table (pages, pool, memory map, Graphics Output, stall, PSCI reset, a console on the tree's UART **and on the screen**, the tree as a configuration table), the PE32+ loader for `loader.efi`; on a Tensor G5 the watchdogs and the framebuffer |
| `font8x8.h` | the screen console's glyphs: Daniel Hepper's font8x8 (public domain) |
| `fdt.c`, `memmap.c` | the device tree reader and the RAM map, host-tested (`make test`, on `tests/frankel-like.dts`) |
| `qemu-test.sh` | the whole route in QEMU, no UEFI firmware anywhere |

The framebuffer, in this order: `scion.fb=BASE,WIDTH,HEIGHT,STRIDE` on the boot image's command line (ABL passes it on in
`/chosen/bootargs`); a `simple-framebuffer` node, should ABL ever add one; else the display controller's enabled layer in
A8R8G8B8, if its SMMU is off (SMMU_CR0.SMMUEN = 0: then the layer's address is physical). The shim prints every layer it
reads either way, and paints a coloured band per stage at the top of the screen (blue: the shim runs; green: the payload is
in place; yellow: the loader starts), so a photograph says how far it got. Below the bands it is the console UEFI's would be:
its own lines and the loader's (the banner, `platform frankel, GICv3`, `CurrentEL=1`, the kernel's window), until the kernel's
own (`-v`) takes the screen; `scion.con=0` turns it off. The framebuffer is reserved in the memory map it gives the loader and
mapped normal non-cacheable, as UEFI maps a GOP's.

The watchdogs: stopped as Google's driver stops them, if their ID register says they are; `scion.wdt=keep` leaves them be.

### What QEMU checks

`shim/qemu-test.sh` runs QEMU virt at EL2 (`virtualization=on`) with a **GICv4** (`gic-version=4`: 256 KB redistributor
frames, as here) and eight cores, the shim as `-kernel`, `loader.efi` and the Pixel 10's kernel (`frankel_defconfig`) in the
payload. QEMU's own tree, its root compatible made `scion,qemu-lga`, selects xnuloader's `platform_qemu_lga`: the Pixel's
GIC, cores and PSCI path at QEMU's addresses. Seen (2026-10-29, QEMU 11.1.2):

| Run | Result |
|---|---|
| `cortex-a76`, 8 cores, one for the kernel | shim at EL2 -> EL1, `loader.efi` loaded and relocated, `platform qemu-lga, GICv3`, XNU to launchd, the run's end back in the loader, the log printed, reset through the shim |
| the same, `cpus=8 bootcpu=2` | the kernel started on core 2 by CPU_ON through the shim's EL2; six cores online through it (the seventh, a "small" one, kept back by design); each core's redistributor 256 KB from the last (`gic_find_redistributor` by GICR_TYPER.VLPIS) |
| `-cpu max` (pointer authentication, fine-grained traps, HCX), `cpus=8 bootcpu=7` | the same: launchd, the return, the reset |
| `virt,gic-version=3` (the shim entered at EL1), `cpus=4 bootcpu=1` | the same, PSCI answered by QEMU |
| `scion.fb=` a framebuffer in guest RAM, the loader's self-test (`--checkpoint 999`), RAM in a file | the screen as the phone's would be: [`shim/tests/qemu-screen.png`](shim/tests/qemu-screen.png), the three bands, the shim's and the loader's lines, the kernel's return |

What QEMU cannot check: QEMU numbers its cores in Aff0 whatever `-smp` says, so the Pixel's Aff1 numbering is not
exercised here (xnuloader's `core_shift = 8`, the POCO's, is); nor anything of the phone itself, ABL above all.

## Bring-up checklist: the SoC's part

What the first runs on a phone have to show about the Tensor G5 itself, in order (the phone's own steps, and how to run
them, are [hakios-frankel's](https://github.com/Scion-Project/hakios-frankel#bring-up-in-order)). Each is a line the shim,
the loader or the kernel prints, on the UART and on the screen.

| # | What | Seen as | If not |
|---|---|---|---|
| 1 | ABL runs the image (`fastboot boot`, AVB-footed) | the screen changes; `shim: ... entered at EL2` | stays in fastboot: the footer, then an LZ4 Image |
| 2 | ABL hands over at EL2 | `entered at EL2` | `entered at EL1 with PSCI by SMC`: `platform_frankel.psci_hvc = 0`, one core only |
| 3 | the RAM and reservations as the tree says | `shim: RAM 0x80000000 + ...`, `0x880000000 + ...`; the free blocks | `/memory` empty: ABL's tree is not what this expects |
| 4 | the watchdogs stopped | `watchdog 0 ... id 0x574454 control ... -> ...` (bit 0 clear) | the phone restarts after 24 or 30 s: pet them instead |
| 5 | the framebuffer | `framebuffer 1080 x 2424 at ...`; the bands and the console on the screen | the DPU's layers printed (UART): `scion.fb=`; an SMMU on (CR0 bit 0): its tables, next steps |
| 6 | a write reaches the panel | the bands show at all | command mode: start a frame (`OUTPUT0_START`), next steps |
| 7 | the loader finds the phone | `platform frankel, GICv3`, `CurrentEL=1` | `platform qemu-virt`: the tree's compatible is not `google,lga-frankel` |
| 8 | the timer | `CNTFRQ=38400000` (inferred; whatever it is, the loader uses it) | `CNTFRQ=0`: `platform_frankel.timer_hz` |
| 9 | the GIC | `gic: v3, ... cpu 0's redistributor ...` | a panic `no redistributor for MPIDR`: the frames' layout |
| 10 | PSCI through the shim | `bootcpu=7 cpus=8`: `7 of 7 processors are running` | `shim: EL2 fault: ESR ...` in the log: what EL1 did that EL2 traps |
| 11 | the RAM log survives a restart | ramoops' pmsg zone (0x95400000) read in Android | ABL clears it: another region, `platform_frankel.ram_log` |

## PixOS: what the Pixel 6a port did

[PixOS-Project](https://github.com/PixOS-Project) (`pixelios-kernel`, MIT; read at `04457e5ddae2`) boots XNU on the Pixel 6a
(Tensor G1, `bluejay`). It is **not built on HakiOS**: it has its own XNU patch series from
[ios-sweet](https://github.com/dipa-studio/ios-sweet) (Redmi Note 10 Pro), its own "SWEET" machine configuration and its
own pre-kernel shim, which builds Apple's device tree and enters XNU itself. Three repositories: `pixelios-kernel` (shim,
patches, `soc/<vendor>/<soc>/`, `device/<vendor>/<codename>/board.json`), `pixelios-userland`, `pixelios-site`. What they
**observed on hardware** and this port takes as facts to check (their `device/google/bluejay/docs/GATE0_RESULTS.md`):

- `fastboot boot` of an **unsigned** header-v4 image: the transfer succeeds and the phone stays in fastboot. With an **AVB
  hash footer** signed by a key of their own making, it runs. So `mkxnuimg.py` adds one (avbtool, a test key).
- ABL enters the kernel at **EL2**; both watchdogs were left running; no framebuffer node in the tree; the framebuffer is
  ABL's (0xFAC00000 on the 6a); the GS101's display needed its DECON trigger restored before a write showed.
- UFS: the host is left up by ABL, but its DMA from low RAM timed out (an S2MPU), the hard part of their port.

No PixOS code is in this repository: the shim is written for HakiOS's loader, from the specifications it cites. Their
findings are cited where they are used.

## Licences

The shim, the configuration and this documentation: MIT (`LICENSE`). `dts/mainline/`: BSD-3-Clause, Google's (its
README). `shim/font8x8.h`: public domain (Daniel Hepper's font8x8, from IBM's and Marcel Sondaar's public domain fonts). Google's GPL-2.0 kernel is a source of facts, named in each comment that uses one; none of its code is here.
