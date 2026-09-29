# KERNEL_RISCV/common: android16-riscv

Changes made for the Android 16 (AOSP, riscv64) bring-up of the BananaPi BPI-F3 (SpacemiT K1) and the BananaPi BPI-SM10 (SpacemiT K3), on branch `android16-riscv`.

Kernel changes for the SpacemiT K1 (BPI-F3) and K3 (BPI-SM10).

## Changes

- **dt-bindings: net: bluetooth: realtek: add RTL8852BS**
- **Bluetooth: hci_h5: add the RTL8852BS**: 3-wire H5 with the Realtek vendor ops; btrtl already knows the 8852B UART firmware.
- **wifi: add the Realtek RTL8852BS SDIO driver**: Out-of-tree Realtek rtl8852bs driver (from Armbian), built as 8852bs.ko with concurrent mode, a dedicated P2P device and dynamic P2P group interfaces for Wi-Fi Direct.
- **mmc: sdhci-of-k1: use a 16-bit access for HOST_CONTROL2**: The 32-bit setbits helper did a misaligned MMIO access that faults on the K1 (SDIO hosts only).
- **mmc: sdhci-of-k1: tolerate missing pinctrl states**: SDIO hosts on a fixed 1.8 V rail have no "uhs" state; selecting the NULL state oopsed.
- **riscv: dts: spacemit: k1: fix the microSD pad group**: mmc1_cfg listed pads 52-57, unrelated GPIOs: the SD pads kept their bootloader setup and pad 52 (I2C4_SDA) was taken from I2C4. Use the MMC1 pads 104-109.
- **riscv: dts: spacemit: bananapi-f3: enable the RTL8852BS Wi-Fi and Bluetooth**: SDIO Wi-Fi on sdhci1 powered through a pwrseq (GPIO67 supply switch, GPIO116 reset), Bluetooth H5 on uart2 clocked from the 48 MHz slow_uart2 source.
- **gpio: spacemit-k1: use regular IRQs**: The bank handler only reads/clears GEDR, so it can run in hard IRQ context. Nested threaded IRQs made request_irq() fail for hard IRQ consumers such as cec-gpio.
- **drm/spacemit: hdmi: feed the CEC notifier**: HDMI-CEC runs on a separate cec-gpio adapter; give it the sink's physical address from the EDID on connect (1.0.0.0 if unreadable).
- **drm/spacemit: hdmi: demote connection logs to debug**
- **riscv: dts: spacemit: bananapi-f3: add HDMI-CEC**: The CEC line reaches GPIO_88: run it as an open-drain cec-gpio adapter linked to the HDMI connector.
- **input: misc: add the SpacemiT P1 power key driver**: Reports the P1 (SPM8821) PMIC power button as KEY_POWER from its press/release interrupts. Ported from the SpacemiT vendor kernel.
- **mfd: simple-mfd-i2c: add the SpacemiT P1 interrupts and power key**: Optional regmap IRQ chip per device; the P1 describes its power key interrupts and adds the power key cell.
- **mfd: simple-mfd-i2c: add the SpacemiT P1 reboot cell**: Without it nothing instantiated spacemit-p1-reboot, and a plain reboot or poweroff did nothing.
- **power: reset: spacemit-p1: use sleep-safe sys-off handlers**: The PMIC is behind I2C, so its handlers sleep: register them on the *_PREPARE modes instead of the atomic power-off/restart chains.
- **drm/spacemit: add a boot splash**: A drm_client draws a compiled-in image once the display is up, until the compositor takes over (fbcon logos are unavailable to module framebuffers on GKI).
- **drm/spacemit: mask underrun interrupt storms**: A wedged pipeline raised ~430k underrun interrupts/s and pinned CPU0 for the whole boot. Mask the source after a burst until the next CRTC enable.
- **drm/spacemit: enable composer layer 0 for primary-only commits**: The composer outputs nothing unless layer 0 is enabled, so commits that only use the primary plane (recovery, boot splash) showed black. Park layer 0 as a transparent solid layer.
- **drm/spacemit: assign rdma channels by pixel format**: Without a userspace rdma_id the plane used rdma = zpos, but only rdma1/rdma3 of the HDMI DPU read raw YUV: a full-screen NV12 video plane was always rejected and every video frame was GPU-composited. Assign the channels per commit, YUV planes first.
- **drm/spacemit_k3: assign rdma channels by pixel format**: Same as the K1 driver; on saturn-hee only rdma1 reads raw YUV.
- **riscv: dts: spacemit: k1: power and place the VPU**: Add the VPU power island (its probe hung the bus without it) and move the VPU into multimedia-bus for the dma-ranges amvx expects.
- **media: spacemit: vpu_k1x: keep the CAPTURE size until the stream is parsed**: The input port is 0x0 before the firmware parses the bitstream; G_FMT returned 0x0 and v4l2_codec2 could not set up its provisional CAPTURE format.
- **media: spacemit: vpu_k1x: set the vb2 queue lock**: vb2 requires q->lock since wait_prepare/wait_finish were removed; REQBUFS failed and the half-initialized queue crashed on stream off.
- **media: spacemit: vpu_k1x: keep the vb2 queue across REQBUFS(0)**: Re-initializing the queue on the next REQBUFS corrupted q->done_wq under a client polling it (list_del corruption on a resolution change).
- **ASoC: es8326: add ADC ramp rate and mic gain properties**: everest,adc-ramp-rate hides the ADC start-up pop with always-on MEMS mics; everest,adc-pga-volume sets the default digital mic gain.
- **riscv: dts: spacemit: bananapi-f3: slow ADC ramp and +18 dB mic gain**
- **riscv: dts: spacemit: bananapi-f3: add the speaker amplifier**: The speakers sit behind an amplifier on the ES8326 headphone output, enabled by GPIO127; model it as a DAPM amplifier.
- **ASoC: spacemit: add the K1 HDMI audio driver**: Drives the ADMA and SSPA directly and polls the DMA position (the completion interrupt goes to the RCPU coprocessor); S16 samples are expanded to IEC subframes. hdmi_jack=0 keeps audio on the built-in speakers.
- **riscv: dts: spacemit: bananapi-f3: add HDMI audio**
- **riscv: dts: spacemit: bananapi-f3: enable the header I2C4 sensors**: I2C4 on header pins 3/5 at 100 kHz with AHT20 and BMP280 nodes; the MPU-9250 IMU is driven from userspace through /dev/i2c-4.
- **media: spacemit: vpu_k3: build as amvx_k3 and port to the current vb2 API**: amvx.ko clashed with the K1 VPU module in the shared K1/K3 config. Use q->lock instead of wait_prepare/wait_finish, fix the memory_model.h include order and make local helpers static.
- **thermal: spacemit: give the K3 driver its own Kconfig symbol**: It reused TI's K3_THERMAL, so enabling it also built the TI bandgap drivers. Add the missing MODULE_LICENSE for the module build.
- **remoteproc: k3-rproc: don't use the unexported frozen()**
- **tee: optee: factor out ARM SMC/HVC conduit helpers**: To allow new architecture to use its own conduit result struct while keeping the core OP-TEE driver code shared, move the SMC and HVC invoke helpers into an ARM specific file and introduce struct optee_conduit_res as a common representation of conduit return values.
- **tee: optee: rename smc_abi.c to optee_abi.c**: Rename to reflect the generic OP-TEE ABI implementation that can support SMC-like interfaces on future non-ARM architectures.
- **tee: optee: make FF-A ABI conditional on CONFIG_ARM_FFA_TRANSPORT**: Only build and register FF-A ABI support when ARM FF-A transport is enable.
- **driver: optee: Support MPXY-based communication for RISC-V**: RISC-V SBI MPXY extension has been implemented as mailbox controller driver. The RISC-V OP-TEE PoC uses the MPXY protocol so OP-TEE driver acts as mailbox client and requests mailbox channels from MPXY mailbox controller.
- **riscv: dts: spacemit: k1: describe OP-TEE over SBI MPXY**: k1-optee.dtsi (BananaPi F3, MusePi Pro): the SBI MPXY mailbox, firmware/optee with method = "mpxy" and one RPMI OP-TEE channel per hart (channel id = hart id), matching the OpenSBI trusted domain set up by pi-u-boot's k1-x-optee.dtsi.
- **riscv: dts: spacemit: k3: add USB3 port B and PCIe port D**: The BPI-SM10 carrier puts its VL817 USB 3.0 hub on USB3 port B (combo PHY shared with PCIe PHY2) and its M.2 M-key 2230 slot on PCIe port D (x1, PHY4). Values from the SpacemiT linux-6.18 BSP.
- **riscv: dts: spacemit: k3: add the CoM260 pin groups**: gmac1 RGMII, i2c0/i2c2/i2c6, SD card (normal and UHS), PCIe ports D and E, DP1 hot-plug, as in the BSP CoM260 board files.
- **riscv: dts: spacemit: add the Banana Pi BPI-SM10**: K3-CoM260 module (k3-com260.dtsi: console, RTL8211F PHY, UFS) on the CoM260 kit V02 carrier (k3-bananapi-sm10.dts: SD, USB-C OTG with FUSB301, VL817 hub, RTL8852BE Wi-Fi/BT, M.2 2230 NVMe, EEPROM, DisplayPort). Not yet: the x4 M.2 slot (two PHYs per controller), fan controller, power button, DSI/CSI.
- **K3 fixes from BayLibre glaroque/k3-display-upstream** (12 commits, Guillaume La Roque): pmdomain devres lifetime, K3 RPMU clock ids + RCPU bus clocks + RPMU syscon, ADMA (no abort while enabling, kzalloc descriptors, abort the previous transfer), k3-ri2s PCM buffer in I2S SRAM, DP1 audio codec playback-only + DP1 audio on Pico-ITX, inno-dp live HPD and ASoC component lifetime. The display rework of that branch is not taken.
- **riscv: dts: spacemit: k3: enable DP audio on the BPI-SM10**: the carrier has no codec; DP audio is the only on-board output.
- **rtc: spacemit-p1: count from the 32.768 kHz crystal**: with the reset value of RTC_CTRL the RTC ran ~5% slow on the BananaPi F3; probe now enables the crystal, its 32k output and the clock select (bits 0, 1, 3), as SpacemiT's vendor driver does.

## Notes

- `drivers/net/wireless/realtek/rtl8852bs` is Realtek's out-of-tree driver (from Armbian).
- OP-TEE: the four `tee: optee:` / `driver: optee:` commits are the RISE RISC-V OP-TEE kernel side (https://gitlab.com/riseproject/riscv-optee/linux, dev-optee-mpxy-v8), ported to 7.1; they match bootloader/spacemit/{opensbi,optee_os}. A different series ("tee: optee: RISC-V support over the RPMI TEE service group") is under review upstream.

## Build

In the Android tree the kernel is checked out in `kernel/spacemit` (local manifest `spacemit-sources.xml`) and built by `build.sh`:

```
./build.sh k1 --kernel-only   # -> device/spacemit/k1-kernel/mainline
./build.sh k3 --kernel-only   # -> device/spacemit/k3-kernel/mainline
```
