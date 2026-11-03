# Mac OS 8.1 test VM (QEMU q800)

The ROM, PRAM and disk images are licensed and are **never committed**. Put them in
`emu/` at the repo root (git-ignored), or point `EMU_DIR` somewhere else:

| File in `emu/` | What it is |
|---|---|
| `q800.rom` | Quadra 800 ROM (checksum F1ACAD13) |
| `pram.qcow2` | PRAM image (QEMU `if=mtd`) |
| `hd.raw` | Mac OS 8.1 boot disk, raw, so `hfsutils` can read and write it while the VM is off |

Setting up from the owner's UTM bundle (`Mac OS 8.1 Claude.utm`):

    qemu-img convert -O raw "<bundle>/Data/42FEA357-….qcow2" emu/hd.raw
    qemu-img convert -O qcow2 "<bundle>/Data/E05FACFA-….qcow2" emu/pram.qcow2
    cp "<bundle>/Data/1993-02 - F1ACAD13 - Quadra, Centris 610,650,800.ROM" emu/q800.rom

Scripts (QEMU ≥ 8.2, `hfsutils`, `python3-pil`):

* `run.sh` boots headless: 32 MB, SONIC NIC on QEMU user networking (the host is
  10.0.2.2), and a monitor socket at `emu/monitor.sock`.
* `mon.sh "<monitor command>"` sends a monitor command, for example
  `screendump /path/x.ppm`, `sendkey ret` or `quit`.
* `boot-test.sh ":HFS:path:to:log"` boots, dismisses the improper-shutdown alert,
  waits for the guest to power off (a test app calls `ShutDwnPower`), and prints
  the log from the disk.

Put test apps in `:System Folder:Startup Items:` using `hcopy -m App.bin`.

## System 7.1 VM

A second VM in `emu/s71/` (also git-ignored) runs a minimal System 7.1 on the same Quadra ROM. Every script takes it through `EMU_DIR`:

    EMU_DIR=$PWD/emu/s71 classic/emu/try-rsync.sh "memory" "rsync --version"

Setting it up from the owner's UTM bundle (`Mac OS 7.1.utm`):

    qemu-img convert -O raw "<bundle>/Data/ED11648F-….qcow2" emu/s71/hd.raw
    qemu-img convert -O qcow2 "<bundle>/Data/F6FB7225-….qcow2" emu/s71/pram.qcow2
    cp "<bundle>/Data/QUAD650.ROM" emu/s71/q800.rom

Checked on 7.1, with MacTCP 2.0.6 (address 10.0.2.15, gateway 10.0.2.2, name server 10.0.2.3):
* **Both apps run.** The GUI draws in black and white at 640×480.
* **macposix self-test:** passes, including the replace that keeps resource forks, and DNS through MacTCP's DNR.
* **Pulls:** with resource forks and type/creator. A second run transfers nothing. `--delete` works on a 2,000-entry tree, with peak use of 0.8 MB.
* **Pushes to the Mac's daemon:** work, and Cmd-. stops it.
* **Startup Items:** 7.1's Finder opens every file in Startup Items, one after another, without waiting for the app to quit. So the scripts mark their helper files invisible, and an unattended run (one with `rsync autoquit`) creates `rsync log` invisible too.
