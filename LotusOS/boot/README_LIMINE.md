# Limine migration (Multiboot2)

This project now boots via **Limine** using the **multiboot2** protocol.

## 1) Get Limine binaries (recommended: binary release)
Example:
- Clone the Limine binary release branch/tag
- Run `make` inside it to build host utilities

Then you need these files:
- `limine` (host utility, for `limine bios-install`)
- `limine-bios.sys`
- `limine-bios-cd.bin`

Optionally for UEFI later:
- `limine-uefi-cd.bin`
- `BOOTX64.EFI`

## 2) Build + run
`make run`

If your Limine files are not in `/usr/share/limine`, set:
`make LIMINE_SHARE=/path/to/limine/share LIMINE_BIN=/path/to/limine`

