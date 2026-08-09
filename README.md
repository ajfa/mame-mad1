# MAME driver for the Mad Computers MAD-1

A MAME driver for the **MAD-1**, the modular IBM PC compatible workstation
built by Mad Computers Inc. of Santa Clara in 1984 around an Intel 80186. It
reviewed well, sold badly, was withdrawn after a year, and is the machine the
French SMT-Goupil G4 was derived from. Nothing emulated it before this.

It boots IBM PC-DOS 3.30 from floppy and is fully interactive; with no disk in
the drive the ROM drops into its own System Monitor.

```
System Memory Size: 256KB                    System Memory Size: 256KB

SELF TESTS COMPLETE                          SELF TESTS COMPLETE
LOADER (840701)...                           LOADER (840701)...
Current date is Tue  1-01-1980               System not found...
Enter new date (mm-dd-yy):                   System Monitor
                                             M>
A>dir

COMMAND  COM    25307   3-17-87  12:00p
ANSI     SYS     1678   3-17-87  12:00p
...
       22 File(s)      9216 bytes free
```

Verified end to end: the power-on diagnostics pass with no failure message,
DOS loads from the floppy, the keyboard works and `DIR` returns the real
catalogue of the disk.

## What you need

The ROM dumps are not redistributed here. `mad_eproms.zip` is attached to post
#7 of the [MAD-1 thread on the VCFed
forums](https://forum.vcfed.org/index.php?threads/mad-1-looking-for-information.1247595/)
by *riktw*; downloading it needs a forum account. Build `mad1.zip` from it:

| file | size | what it is |
|---|---|---|
| `e.bin` | 8K | motherboard EPROM, even bytes |
| `o.bin` | 8K | motherboard EPROM, odd bytes |
| `gpu.bin` | 8K | character generator on the video board |
| `hdd.bin` | 4K | hard disk controller EPROM (not used yet) |
| `5788005.u33` | 8K | a copy of `gpu.bin` under the name MAME's CGA card wants |

Any 360K MS-DOS or PC-DOS 2.x-3.3 disk image boots. `tools/imd2img.py`
converts ImageDisk `.imd` files to raw images, though MAME reads `.imd`
directly as well.

The [schematic and the 165 page technical
reference](https://bitsavers.org/pdf/madComputer/mad-1/) are on bitsavers.

## Building

```sh
git clone https://github.com/mamedev/mame.git
mkdir -p mame/src/mame/madcomputer
cp src/mad1.cpp mame/src/mame/madcomputer/
# add a "@source:madcomputer/mad1.cpp" / "mad1" stanza to src/mame/mame.lst
cd mame && make SUBTARGET=mad1 SOURCES=src/mame/madcomputer/mad1.cpp -j3
```

Then `scripts/run.sh` boots the machine, and `scripts/run.sh --check` does it
headless and verifies the result. On a machine with 8 GB of RAM do not go
above `-j3`: the build runs out of memory, silently truncates `luaengine.o`
and then fails to link with `undefined reference to lua_engine::`.

## What is missing

* **The video board is a stand-in.** MAME's stock ISA CGA card is used, fed
  the MAD-1's own character generator under the IBM font ROM's name (MAME
  prints a checksum warning). The real board is a single 6845 card with both
  monochrome and colour modes and two fonts in its EPROM; it deserves its own
  device. The driver is flagged `MACHINE_IMPERFECT_GRAPHICS` for this.
* **Channel 1 of the timer, the DRAM refresh, is not wired to the DMA
  controller.** Nothing in the power-on diagnostics checks it.
* **The keyboard is a high level emulation.** The real unit is 8048 based and
  its ROM has never been dumped, and none of MAME's XT keyboards can stand in
  (`pcxt83` needs `4584751.m1`, `pc83` has its MCU as `NO_DUMP`). Scan codes
  are injected into port A directly.
* **No hard disk.** `hdd.bin` is in the ROM set but nothing drives it. The
  controller is IBM compatible (Xebec, `WD1002-WX1` is reported to work in the
  real machine) at `320-32F`, and SW1 already selects booting from it.
* **The second serial port, the RTC and the parity logic** are not modelled
  beyond what the POST needs.
* The CPU clock is a guess: the technical reference never states it. 16 MHz
  crystal, i.e. 8 MHz, is consistent with the documented 4 MHz DMA clock.

## Notes for anyone picking this up

**The 16K EPROM has to be mirrored.** The reset vector at `FFFF0` programs the
80186 chip selects and jumps to `F800:0000`:

| register | value | meaning |
|---|---|---|
| UMCS `FFA0` | `F83A` | 32K ROM window at `F8000` |
| LMCS `FFA2` | `3FF8` | 256K of low RAM |
| MMCS `FFA6` | `81F9` | mid-range at `80000` |
| MPCS `FFA8` | `A039` | peripherals in I/O space |

The window is 32K but the EPROM is 16K, so it appears twice — the far jump
lands on the first copy while the reset vector lives in the second:
`map(0xf8000, 0xfbfff).mirror(0x4000)`. The code then sets the M/S bit of the
relocation register (`FFFE |= 0x4000`), which puts the 80186's own interrupt
controller in slave mode behind the external 8259A, so the driver needs
`set_irmx_irq_ack`.

**The 8255 is not wired like an IBM PC's, and that is what keeps the disk from
working.** Section 2.8.1 of the technical reference:

* **port A (60h, read)** — keyboard scan code, or the configuration switches
  when bit 7 of port B is set. That is the 5150 arrangement, not the 5160's.
* **port B (61h, write)** — bits 0/1 speaker, bit 2 floppy motor (0 = on),
  bit 3 RAM parity enable, bit 4 I/O parity enable, bit 5 keyboard clock,
  bit 7 read the configuration switches.
* **port C (62h)** — bits 6-7 are the parity error flags and **must read back
  clear** or the POST aborts with `RAM parity error detected`; and the BIOS
  *writes* it to pulse the 8272's reset line:

  ```
  in al,62h ; or al,0Ch ; out 62h,al ; delay ; and al,0F7h ; out 62h,al
  ```

Neither `ibm5150_mb_device` (switches on port A, but port C is input only) nor
`ibm5160_mb_device` (switches on port C) can express that. Until the driver
supplied its own 8255 the BIOS never touched `3F0-3F7` at all: it reset the
controller through a port nothing was listening on and then sat waiting for an
interrupt that could not arrive. The driver maps its own `i8255_device` over
`0060-0063` on top of the motherboard device's map, and translates port C into
the digital output register of MAME's XT floppy card.

**SW2 is not unused.** The technical reference says *"SW 2: Not Used (Set to On
Position)"*. The last power-on routine at `F85C4` disagrees:

```
call F850E        ; read the configuration switches into AH
test ah,2
jz   F85F4        ; -> int 19h, boot
jmp  F804E        ; -> restart the diagnostics from the top
```

With that bit set the machine loops the diagnostics forever. It is a burn-in
switch, and it cost an afternoon of looking for a phantom hang.

**The display switches in the manual are swapped.** Bits 4-5 of the switch
byte: `00` = no monitor, `01` = colour 40x25, `10` = colour 80x25, `11` =
monochrome — the manual has `01` and `10` the other way round. `00` is worth
remembering: it sends the console to COM1, and the ROM has a serial character
output routine at `F8164` (9600 8N1, set up from the table at `022B`). The
POST normally writes to the CRTC, but its *error* messages go out of the
serial port as well.

**Shadowing the motherboard device's 8255 makes the machine whistle.** Its
speaker is driven by `m_pc_spkrdata & m_pit_out2`, and the gate of channel 2 of
its 8253 comes from port B of the 8255 it owns. Take that 8255 over and the
gate is left floating, the counter free runs and the machine emits a continuous
tone from the first frame. `MACHINE_NO_SOUND` does not help — it is a label,
not a mute. Driving the gate from outside is not enough either, because the
speaker data bit goes through a protected member. The way out is the same trick
as for the 8255: the driver instantiates its own 8254 and its own speaker and
maps them over `0040-0043`. The motherboard device's 8253 is then never
programmed, its output never toggles and it stays quiet — and the MAD-1's
speaker works properly, power-on beeps included.

**`gpu.bin` is not a GPU.** It is the character generator: four 2K banks, of
which 0 and 1 are rows 0-7 and 8-15 of an 8x16 font, 2 is all ones, and 3 is
an 8x8 font.

**Instrumenting this under MAME.** `-str N` writes a snapshot at the end even
with `-video none`, and that is the only reliable way to read the screen
headless — Lua frame notifiers stop firing without a video backend and the
machine stop notifier is not always reached. Do **not** put an
`install_write_tap` on the video card's memory (the card reinstalls its
handlers on a mode change and MAME crashes), and do not read memory from
inside a tap. `-debugger none` swallows the `printf` output of breakpoint
actions, but `trace file,maincpu` issued from Lua works and compresses loops,
so seventy seconds of power-on self test fit in a readable file. Writing to
the I/O space from inside a device handler re-enters the address space, so the
driver defers the floppy DOR write with a zero delay timer.

`-autoboot_command` takes MAME's own escape sequences: Enter is the two
characters `\n`, not a real newline. And do not redirect the emulator's stdin
from `/dev/null` in a self test — MAME then never posts the keystrokes.

## Credits

* ROM dumps: *riktw* on the VCFed forums.
* Schematic and technical reference: [bitsavers](https://bitsavers.org/pdf/madComputer/mad-1/).
* MAME: MAMEdev.

## Licence

The driver and the tools in this repository are BSD-3-Clause, the same licence
MAME uses. See [LICENSE](LICENSE).
