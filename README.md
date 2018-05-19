# romdump ![License](https://img.shields.io/badge/License-GPLv2-blue.svg)
Dumps the RAW FUSE, KFUSE and BOOTROM bytes to your microSD/HOST PC via USB/console screen

## Usage
 1. Build `romdump.bin` using make from the repository root directory, or download a binary release from https://switchtools.sshnuke.net
 2. Send the romdump.bin to your Switch running in RCM mode via a fusee-launcher (sudo ./fusee-launcher.py romdump.bin or just drag and drop it onto TegraRcmSmash.exe on Windows)
 3. Read the screen for warnings/errors (fuse bytes will also be printed here, however its much better to read them out from the microSD or via USB with TegraRcmSmash.exe -r)

## Changes

This section is required by the GPLv2 license

 * initial code based on https://github.com/Atmosphere-NX/Atmosphere
 * everything except fusee-primary been removed (from Atmosphere)
 * all hwinit code has been replaced by the updated versions from https://github.com/nwert/hekate
 * Files pinmux.c/h, carveout.c/h, flow.h, sdram.c/h, decomp.h,lz4_wrapper.c,lzma.c,lzmadecode.c,lz4.c.inc are based on https://github.com/fail0verflow/switch-coreboot.git sources
 * main.c has been modified to readout fuse/kfuse data, store them to a USB buffer, write them to microSD files (if inserted), and send the USB buffer if the host is listening.

## Responsibility

**I am not responsible for anything, including dead switches, loss of life, or total nuclear annihilation.**
