// license:BSD-3-Clause
// copyright-holders:ajfa
/***************************************************************************

    Mad Computers Inc. MAD-1 (1984)

    A modular IBM PC compatible workstation from Santa Clara, CA, built
    around an Intel 80186 running in "slave" mode with a conventional
    IBM PC/XT peripheral complement.  It was the technical ancestor of
    the French SMT-Goupil G4.

    Hardware (per the MAD-1 Technical Reference Manual, 840707:20.07,
    bitsavers.org/pdf/madComputer/mad-1):

    - Intel 80186 in minimum mode
    - 128K/256K RAM on the CPU board (512K max), 16K EPROM
      (2x 2764; option for 2x 27128 = 32K)
    - Intel 8237A-5 DMA (4 MHz clock)
    - Intel 8259A-2 PIC (the 80186 is put in slave mode, so the
      external PIC supplies the interrupt vectors)
    - Intel 8254 timer at 1.19 MHz
    - Intel 8255A-5 PPI, see below
    - Intel 8272A floppy disk controller
    - 2x Intel 8250 UART (primary 3F8, secondary 2F8)
    - MC6845 CRTC on the video board, monochrome and colour capable,
      with an 8K character generator EPROM holding an 8x16 and an 8x8 font
    - Intel 8048 based 83-key keyboard, PC/XT style serial interface
    - Optional 10MB hard disk on an IBM-compatible (Xebec) controller

    The I/O map is that of the IBM PC (see Appendix B of the technical
    reference); the only documented deviation is the NMI mask register,
    which lives at 0xa0 and is written with 0x80 to enable and 0x00 to
    disable NMI.

    The boot vector at FFFF0 programs the 80186 chip selects
    (UMCS=F83A -> 32K ROM window at F8000, LMCS=3FF8 -> 256K RAM,
    MMCS=81F9, MPCS=A039) and jumps to F800:0000, so the 16K EPROM
    appears twice in the upper 32K.

    The 8255 is *not* wired like an IBM PC's, which is why the generic
    motherboard device cannot drive the disk or the keyboard (section 2.8.1
    of the technical reference):

      port A (60h, read)   - keyboard scan code, or the configuration
                             switches when port B bit 7 is set
      port B (61h, write)  - 0/1 speaker, 2 floppy motor (0 = on),
                             3 RAM parity enable, 4 I/O parity enable,
                             5 keyboard clock, 7 read configuration switches
      port C (62h)         - written by the BIOS; bit 3 pulses the floppy
                             controller's reset line

    Because the motherboard device's own 8255 is shadowed, the gate of channel
    2 of its 8253 would be left floating and its speaker would whistle
    continuously, so this driver brings its own 8254 and speaker too and maps
    them over 0040-0043.

    The configuration switches (port A with port B bit 7 set) are also
    MAD-1 specific: bit 0 selects floppy (1) or hard disk (0) boot,
    bits 2-3 are the memory size, bits 4-5 the display (00 = none, so the
    console goes to COM1, 01 = colour 80x25, 10 = colour 40x25,
    11 = monochrome) and bits 6-7 the number of floppy drives.

    Boots PC-DOS 3.30 from floppy and takes keyboard input.

    Note that SW2, which the technical reference calls unused, is tested by
    the last power-on diagnostic: when it is set the machine loops the
    diagnostics forever instead of booting.

    TODO:
    - the video board is emulated with a stock ISA CGA card fed the MAD-1's
      own character generator; a proper device for it is needed
    - channel 1 of the timer, the DRAM refresh, is not wired to the DMA
      controller; nothing in the diagnostics checks it
    - the keyboard is a high level emulation; the real one is an 8048 unit
      whose ROM has not been dumped
    - hard disk controller EPROM (hdd.bin) is not used yet

***************************************************************************/

#include "emu.h"

#include "bus/isa/isa.h"
#include "bus/isa/isa_cards.h"
#include "cpu/i86/i186.h"
#include "machine/genpc.h"
#include "machine/i8255.h"
#include "machine/pic8259.h"
#include "machine/pit8253.h"
#include "machine/ram.h"
#include "sound/spkrdev.h"

#include "speaker.h"

#include "softlist_dev.h"


namespace {

class mad1_state : public driver_device
{
public:
	mad1_state(const machine_config &mconfig, device_type type, const char *tag) :
		driver_device(mconfig, type, tag),
		m_maincpu(*this, "maincpu"),
		m_ppi(*this, "ppi"),
		m_pic(*this, "mb:pic8259"),
		m_pit(*this, "pit"),
		m_speaker(*this, "speaker"),
		m_dsw(*this, "DSW"),
		m_keys(*this, "ROW%u", 0U)
	{ }

	void mad1(machine_config &config);

protected:
	virtual void machine_start() override ATTR_COLD;
	virtual void machine_reset() override ATTR_COLD;

private:
	required_device<i80186_cpu_device> m_maincpu;
	required_device<i8255_device> m_ppi;
	required_device<pic8259_device> m_pic;
	required_device<pit8254_device> m_pit;
	required_device<speaker_sound_device> m_speaker;
	required_ioport m_dsw;
	required_ioport_array<11> m_keys;

	void mad1_map(address_map &map) ATTR_COLD;
	void mad1_io(address_map &map) ATTR_COLD;

	uint8_t ppi_porta_r();
	void ppi_portb_w(uint8_t data);
	uint8_t ppi_portc_r();
	void ppi_portc_w(uint8_t data);
	void update_fdc();
	void pit_out2_w(int state);

	TIMER_CALLBACK_MEMBER(scan_keyboard);
	TIMER_CALLBACK_MEMBER(update_fdc_deferred);

	uint8_t m_portb = 0xff;
	uint8_t m_portc = 0xff;
	uint8_t m_scancode = 0;
	int m_pit_out2 = 0;
	bool m_irq1 = false;

	emu_timer *m_kbd_timer = nullptr;
	emu_timer *m_dor_timer = nullptr;
	uint8_t m_keystate[11]{};
	uint8_t m_queue[32]{};
	uint8_t m_qhead = 0, m_qtail = 0;

	void queue(uint8_t code)
	{
		uint8_t next = (m_qtail + 1) & 31;
		if (next != m_qhead)
		{
			m_queue[m_qtail] = code;
			m_qtail = next;
		}
	}
};


void mad1_state::mad1_map(address_map &map)
{
	map.unmap_value_high();
	// 16K EPROM in a 32K chip select window at F8000, so it appears twice
	map(0xf8000, 0xfbfff).mirror(0x4000).rom().region("bios", 0);
}

void mad1_state::mad1_io(address_map &map)
{
	map.unmap_value_high();
	map(0x0000, 0x00ff).m("mb", FUNC(ibm5160_mb_device::map));
	// our 8254 and 8255 replace the motherboard device's
	map(0x0040, 0x0043).rw(m_pit, FUNC(pit8254_device::read), FUNC(pit8254_device::write));
	map(0x0060, 0x0063).rw(m_ppi, FUNC(i8255_device::read), FUNC(i8255_device::write));
}


uint8_t mad1_state::ppi_porta_r()
{
	if (BIT(m_portb, 7))
		return m_dsw->read();

	// the BIOS acknowledges a scan code by reading it; hand over the next
	// one on the following scan
	uint8_t const code = m_scancode;
	if (!machine().side_effects_disabled() && m_irq1)
	{
		m_irq1 = false;
		m_pic->ir1_w(0);
	}
	return code;
}

void mad1_state::ppi_portb_w(uint8_t data)
{
	m_portb = data;
	// bits 0 and 1 are the gate of channel 2 of the timer and the speaker
	// data, exactly as on a PC
	m_pit->write_gate2(BIT(data, 0));
	m_speaker->level_w(m_pit_out2 & BIT(data, 1));
	update_fdc();
}

uint8_t mad1_state::ppi_portc_r()
{
	// bit 0 high = mini floppy; bits 6-7 are the RAM and I/O parity error
	// flags and must read back clear or the POST stops with
	// "RAM parity error detected"
	return (m_portc & 0x0f) | 0x01;
}

void mad1_state::ppi_portc_w(uint8_t data)
{
	m_portc = data;
	update_fdc();
}

void mad1_state::pit_out2_w(int state)
{
	m_pit_out2 = state;
	m_speaker->level_w(m_pit_out2 & BIT(m_portb, 1));
}

void mad1_state::update_fdc()
{
	m_dor_timer->adjust(attotime::zero);
}

TIMER_CALLBACK_MEMBER(mad1_state::update_fdc_deferred)
{
	uint8_t dor = 0x08;             // interrupts and DMA always enabled
	if (!BIT(m_portc, 3))
		dor |= 0x04;                // port C bit 3 high holds the 8272 reset
	if (!BIT(m_portb, 2))
		dor |= 0x10 | 0x20;         // port B bit 2 low turns the motors on

	// the XT floppy card exposes its digital output register at 3F2; on the
	// MAD-1 those lines come from the 8255 instead
	m_maincpu->space(AS_IO).write_byte(0x3f2, dor);
}


TIMER_CALLBACK_MEMBER(mad1_state::scan_keyboard)
{
	for (int row = 0; row < 11; row++)
	{
		uint8_t const state = m_keys[row]->read();
		uint8_t const diff = state ^ m_keystate[row];
		if (diff)
		{
			for (int bit = 0; bit < 8; bit++)
			{
				if (BIT(diff, bit))
				{
					uint8_t const code = row * 8 + bit + 1;
					queue(BIT(state, bit) ? code : (code | 0x80));
				}
			}
			m_keystate[row] = state;
		}
	}

	if (!m_irq1 && m_qhead != m_qtail && !BIT(m_portb, 7))
	{
		m_scancode = m_queue[m_qhead];
		m_qhead = (m_qhead + 1) & 31;
		m_irq1 = true;
		m_pic->ir1_w(1);
	}
}


void mad1_state::machine_start()
{
	m_dor_timer = timer_alloc(FUNC(mad1_state::update_fdc_deferred), this);
	m_kbd_timer = timer_alloc(FUNC(mad1_state::scan_keyboard), this);
	m_kbd_timer->adjust(attotime::from_msec(5), 0, attotime::from_msec(5));

	save_item(NAME(m_portb));
	save_item(NAME(m_portc));
	save_item(NAME(m_scancode));
	save_item(NAME(m_pit_out2));
	save_item(NAME(m_irq1));
	save_item(NAME(m_keystate));
	save_item(NAME(m_queue));
	save_item(NAME(m_qhead));
	save_item(NAME(m_qtail));
}

void mad1_state::machine_reset()
{
	m_portb = 0xff;
	m_portc = 0xff;
	m_scancode = 0;
	m_irq1 = false;
	m_qhead = m_qtail = 0;
	std::fill(std::begin(m_keystate), std::end(m_keystate), 0);
}


static INPUT_PORTS_START( mad1 )
	// bit b of ROWn is XT scan code n*8 + b + 1
	PORT_START("ROW0")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_ESC) PORT_NAME("Esc") PORT_CHAR(UCHAR_MAMEKEY(ESC))
	PORT_BIT(0x02, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_1) PORT_NAME("1 !") PORT_CHAR('1') PORT_CHAR('!')
	PORT_BIT(0x04, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_2) PORT_NAME("2 @") PORT_CHAR('2') PORT_CHAR('@')
	PORT_BIT(0x08, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_3) PORT_NAME("3 #") PORT_CHAR('3') PORT_CHAR('#')
	PORT_BIT(0x10, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_4) PORT_NAME("4 $") PORT_CHAR('4') PORT_CHAR('$')
	PORT_BIT(0x20, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_5) PORT_NAME("5 %") PORT_CHAR('5') PORT_CHAR('%')
	PORT_BIT(0x40, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_6) PORT_NAME("6 ^") PORT_CHAR('6') PORT_CHAR('^')
	PORT_BIT(0x80, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_7) PORT_NAME("7 &") PORT_CHAR('7') PORT_CHAR('&')

	PORT_START("ROW1")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_8) PORT_NAME("8 *") PORT_CHAR('8') PORT_CHAR('*')
	PORT_BIT(0x02, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_9) PORT_NAME("9 (") PORT_CHAR('9') PORT_CHAR('(')
	PORT_BIT(0x04, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_0) PORT_NAME("0 )") PORT_CHAR('0') PORT_CHAR(')')
	PORT_BIT(0x08, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_MINUS) PORT_NAME("- _") PORT_CHAR('-') PORT_CHAR('_')
	PORT_BIT(0x10, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_EQUALS) PORT_NAME("= +") PORT_CHAR('=') PORT_CHAR('+')
	PORT_BIT(0x20, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_BACKSPACE) PORT_NAME("Backspace") PORT_CHAR(8)
	PORT_BIT(0x40, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_TAB) PORT_NAME("Tab") PORT_CHAR(9)
	PORT_BIT(0x80, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_Q) PORT_NAME("Q") PORT_CHAR('q') PORT_CHAR('Q')

	PORT_START("ROW2")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_W) PORT_NAME("W") PORT_CHAR('w') PORT_CHAR('W')
	PORT_BIT(0x02, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_E) PORT_NAME("E") PORT_CHAR('e') PORT_CHAR('E')
	PORT_BIT(0x04, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_R) PORT_NAME("R") PORT_CHAR('r') PORT_CHAR('R')
	PORT_BIT(0x08, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_T) PORT_NAME("T") PORT_CHAR('t') PORT_CHAR('T')
	PORT_BIT(0x10, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_Y) PORT_NAME("Y") PORT_CHAR('y') PORT_CHAR('Y')
	PORT_BIT(0x20, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_U) PORT_NAME("U") PORT_CHAR('u') PORT_CHAR('U')
	PORT_BIT(0x40, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_I) PORT_NAME("I") PORT_CHAR('i') PORT_CHAR('I')
	PORT_BIT(0x80, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_O) PORT_NAME("O") PORT_CHAR('o') PORT_CHAR('O')

	PORT_START("ROW3")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_P) PORT_NAME("P") PORT_CHAR('p') PORT_CHAR('P')
	PORT_BIT(0x02, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_OPENBRACE) PORT_NAME("[ {") PORT_CHAR('[') PORT_CHAR('{')
	PORT_BIT(0x04, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_CLOSEBRACE) PORT_NAME("] }") PORT_CHAR(']') PORT_CHAR('}')
	PORT_BIT(0x08, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_ENTER) PORT_NAME("Enter") PORT_CHAR(13)
	PORT_BIT(0x10, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_LCONTROL) PORT_NAME("Ctrl") PORT_CHAR(UCHAR_SHIFT_2)
	PORT_BIT(0x20, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_A) PORT_NAME("A") PORT_CHAR('a') PORT_CHAR('A')
	PORT_BIT(0x40, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_S) PORT_NAME("S") PORT_CHAR('s') PORT_CHAR('S')
	PORT_BIT(0x80, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_D) PORT_NAME("D") PORT_CHAR('d') PORT_CHAR('D')

	PORT_START("ROW4")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_F) PORT_NAME("F") PORT_CHAR('f') PORT_CHAR('F')
	PORT_BIT(0x02, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_G) PORT_NAME("G") PORT_CHAR('g') PORT_CHAR('G')
	PORT_BIT(0x04, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_H) PORT_NAME("H") PORT_CHAR('h') PORT_CHAR('H')
	PORT_BIT(0x08, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_J) PORT_NAME("J") PORT_CHAR('j') PORT_CHAR('J')
	PORT_BIT(0x10, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_K) PORT_NAME("K") PORT_CHAR('k') PORT_CHAR('K')
	PORT_BIT(0x20, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_L) PORT_NAME("L") PORT_CHAR('l') PORT_CHAR('L')
	PORT_BIT(0x40, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_COLON) PORT_NAME("; :") PORT_CHAR(';') PORT_CHAR(':')
	PORT_BIT(0x80, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_QUOTE) PORT_NAME("' \"") PORT_CHAR('\'') PORT_CHAR('"')

	PORT_START("ROW5")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_TILDE) PORT_NAME("` ~") PORT_CHAR('`') PORT_CHAR('~')
	PORT_BIT(0x02, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_LSHIFT) PORT_NAME("Left Shift") PORT_CHAR(UCHAR_SHIFT_1)
	PORT_BIT(0x04, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_BACKSLASH) PORT_NAME("\\ |") PORT_CHAR('\\') PORT_CHAR('|')
	PORT_BIT(0x08, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_Z) PORT_NAME("Z") PORT_CHAR('z') PORT_CHAR('Z')
	PORT_BIT(0x10, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_X) PORT_NAME("X") PORT_CHAR('x') PORT_CHAR('X')
	PORT_BIT(0x20, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_C) PORT_NAME("C") PORT_CHAR('c') PORT_CHAR('C')
	PORT_BIT(0x40, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_V) PORT_NAME("V") PORT_CHAR('v') PORT_CHAR('V')
	PORT_BIT(0x80, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_B) PORT_NAME("B") PORT_CHAR('b') PORT_CHAR('B')

	PORT_START("ROW6")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_N) PORT_NAME("N") PORT_CHAR('n') PORT_CHAR('N')
	PORT_BIT(0x02, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_M) PORT_NAME("M") PORT_CHAR('m') PORT_CHAR('M')
	PORT_BIT(0x04, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_COMMA) PORT_NAME(", <") PORT_CHAR(',') PORT_CHAR('<')
	PORT_BIT(0x08, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_STOP) PORT_NAME(". >") PORT_CHAR('.') PORT_CHAR('>')
	PORT_BIT(0x10, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_SLASH) PORT_NAME("/ ?") PORT_CHAR('/') PORT_CHAR('?')
	PORT_BIT(0x20, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_RSHIFT) PORT_NAME("Right Shift")
	PORT_BIT(0x40, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_ASTERISK) PORT_NAME("KP *")
	PORT_BIT(0x80, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_LALT) PORT_NAME("Alt")

	PORT_START("ROW7")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_SPACE) PORT_NAME("Space") PORT_CHAR(' ')
	PORT_BIT(0x02, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_CAPSLOCK) PORT_NAME("Caps Lock")
	PORT_BIT(0x04, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_F1) PORT_NAME("F1") PORT_CHAR(UCHAR_MAMEKEY(F1))
	PORT_BIT(0x08, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_F2) PORT_NAME("F2") PORT_CHAR(UCHAR_MAMEKEY(F2))
	PORT_BIT(0x10, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_F3) PORT_NAME("F3") PORT_CHAR(UCHAR_MAMEKEY(F3))
	PORT_BIT(0x20, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_F4) PORT_NAME("F4") PORT_CHAR(UCHAR_MAMEKEY(F4))
	PORT_BIT(0x40, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_F5) PORT_NAME("F5") PORT_CHAR(UCHAR_MAMEKEY(F5))
	PORT_BIT(0x80, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_F6) PORT_NAME("F6") PORT_CHAR(UCHAR_MAMEKEY(F6))

	PORT_START("ROW8")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_F7) PORT_NAME("F7") PORT_CHAR(UCHAR_MAMEKEY(F7))
	PORT_BIT(0x02, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_F8) PORT_NAME("F8") PORT_CHAR(UCHAR_MAMEKEY(F8))
	PORT_BIT(0x04, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_F9) PORT_NAME("F9") PORT_CHAR(UCHAR_MAMEKEY(F9))
	PORT_BIT(0x08, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_F10) PORT_NAME("F10") PORT_CHAR(UCHAR_MAMEKEY(F10))
	PORT_BIT(0x10, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_NUMLOCK) PORT_NAME("Num Lock")
	PORT_BIT(0x20, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_SCRLOCK) PORT_NAME("Scroll Lock")
	PORT_BIT(0x40, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_7_PAD) PORT_NAME("KP 7")
	PORT_BIT(0x80, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_8_PAD) PORT_NAME("KP 8")

	PORT_START("ROW9")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_9_PAD) PORT_NAME("KP 9")
	PORT_BIT(0x02, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_MINUS_PAD) PORT_NAME("KP -")
	PORT_BIT(0x04, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_4_PAD) PORT_NAME("KP 4")
	PORT_BIT(0x08, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_5_PAD) PORT_NAME("KP 5")
	PORT_BIT(0x10, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_6_PAD) PORT_NAME("KP 6")
	PORT_BIT(0x20, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_PLUS_PAD) PORT_NAME("KP +")
	PORT_BIT(0x40, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_1_PAD) PORT_NAME("KP 1")
	PORT_BIT(0x80, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_2_PAD) PORT_NAME("KP 2")

	PORT_START("ROW10")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_3_PAD) PORT_NAME("KP 3")
	PORT_BIT(0x02, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_0_PAD) PORT_NAME("KP 0")
	PORT_BIT(0x04, IP_ACTIVE_HIGH, IPT_KEYBOARD) PORT_CODE(KEYCODE_DEL_PAD) PORT_NAME("KP .")
	PORT_BIT(0xf8, IP_ACTIVE_HIGH, IPT_UNUSED)
	PORT_START("DSW")
	PORT_DIPNAME(0x01, 0x01, "Boot device") PORT_DIPLOCATION("SW:1")
	PORT_DIPSETTING(   0x00, "Hard disk")
	PORT_DIPSETTING(   0x01, "Floppy disk")
	// the technical reference calls SW2 unused, but the last POST routine
	// tests it and jumps back to the start of the diagnostics when it is set
	PORT_DIPNAME(0x02, 0x00, "Loop diagnostics") PORT_DIPLOCATION("SW:2")
	PORT_DIPSETTING(   0x00, DEF_STR(No))
	PORT_DIPSETTING(   0x02, DEF_STR(Yes))
	PORT_DIPNAME(0x0c, 0x04, "Memory size") PORT_DIPLOCATION("SW:3,4")
	PORT_DIPSETTING(   0x00, "128K")
	PORT_DIPSETTING(   0x04, "256K")
	PORT_DIPSETTING(   0x08, "384K")
	PORT_DIPNAME(0x30, 0x20, "Display") PORT_DIPLOCATION("SW:5,6")
	PORT_DIPSETTING(   0x00, "None (console on COM1)")
	PORT_DIPSETTING(   0x10, "Colour 40x25")
	PORT_DIPSETTING(   0x20, "Colour 80x25")
	PORT_DIPSETTING(   0x30, "Monochrome")
	PORT_DIPNAME(0xc0, 0x40, "Floppy drives") PORT_DIPLOCATION("SW:7,8")
	PORT_DIPSETTING(   0x00, "1")
	PORT_DIPSETTING(   0x40, "2")
	PORT_DIPSETTING(   0x80, "3")
	PORT_DIPSETTING(   0xc0, "4")
INPUT_PORTS_END


void mad1_state::mad1(machine_config &config)
{
	I80186(config, m_maincpu, 16_MHz_XTAL);
	m_maincpu->set_addrmap(AS_PROGRAM, &mad1_state::mad1_map);
	m_maincpu->set_addrmap(AS_IO, &mad1_state::mad1_io);
	// the reset code sets the M/S bit in the relocation register, i.e. the
	// on-chip interrupt controller runs in slave mode behind the 8259A-2
	m_maincpu->set_irmx_irq_ack("mb:pic8259", FUNC(pic8259_device::inta_cb));

	ibm5160_mb_device &mb(IBM5160_MOTHERBOARD(config, "mb"));
	mb.set_cputag(m_maincpu);
	mb.int_callback().set(m_maincpu, FUNC(i80186_cpu_device::int0_w));
	mb.nmi_callback().set_inputline(m_maincpu, INPUT_LINE_NMI);

	// the MAD-1's own 8254 and speaker, mapped over the motherboard device's
	// 8253: that one cannot be reached, and its speaker sits on a free running
	// counter and whistles
	PIT8254(config, m_pit);
	m_pit->set_clk<0>(XTAL(14'318'181) / 12.0);
	m_pit->out_handler<0>().set(m_pic, FUNC(pic8259_device::ir0_w));
	m_pit->set_clk<1>(XTAL(14'318'181) / 12.0);
	m_pit->set_clk<2>(XTAL(14'318'181) / 12.0);
	m_pit->out_handler<2>().set(FUNC(mad1_state::pit_out2_w));

	SPEAKER(config, "mono").front_center();
	SPEAKER_SOUND(config, m_speaker).add_route(ALL_OUTPUTS, "mono", 0.80);

	I8255(config, m_ppi);
	m_ppi->in_pa_callback().set(FUNC(mad1_state::ppi_porta_r));
	m_ppi->out_pb_callback().set(FUNC(mad1_state::ppi_portb_w));
	m_ppi->in_pc_callback().set(FUNC(mad1_state::ppi_portc_r));
	m_ppi->out_pc_callback().set(FUNC(mad1_state::ppi_portc_w));

	ISA8_SLOT(config, "isa1", 0, "mb:isa", pc_isa8_cards, "cga", false);   // on-board video
	ISA8_SLOT(config, "isa2", 0, "mb:isa", pc_isa8_cards, "fdc_xt", false); // on-board 8272A
	ISA8_SLOT(config, "isa3", 0, "mb:isa", pc_isa8_cards, "lpt", false);
	ISA8_SLOT(config, "isa4", 0, "mb:isa", pc_isa8_cards, "com", false);
	ISA8_SLOT(config, "isa5", 0, "mb:isa", pc_isa8_cards, nullptr, false);

	RAM(config, RAM_TAG).set_default_size("256K").set_extra_options("128K, 384K, 512K");

	SOFTWARE_LIST(config, "disk_list").set_original("ibm5150");
}


ROM_START( mad1 )
	ROM_REGION16_LE(0x4000, "bios", 0)
	ROMX_LOAD("e.bin", 0x0000, 0x2000, CRC(a7af0b1a) SHA1(3e92313a9b3a9f595d1191b8355164376f974d3c), ROM_SKIP(1))
	ROMX_LOAD("o.bin", 0x0001, 0x2000, CRC(b9bdf0c0) SHA1(804feca9d88669e72b418f27e27d03d8074dac26), ROM_SKIP(1))

	// character generator on the video board: 8x16 font in the first two
	// 2K banks, an all-ones bank, then an 8x8 font
	ROM_REGION(0x2000, "chargen", 0)
	ROM_LOAD("gpu.bin", 0x0000, 0x2000, CRC(b8ce2dfd) SHA1(8617ec1066daa08bb35cc1d028b0774355cbe443))

	// hard disk controller
	ROM_REGION(0x1000, "hdc", 0)
	ROM_LOAD("hdd.bin", 0x0000, 0x1000, CRC(d5ad94f7) SHA1(748c5087ef1b9d27fa44e9abb29476504f1bed41))
ROM_END

} // anonymous namespace


//    YEAR  NAME  PARENT  COMPAT  MACHINE  INPUT  CLASS       INIT        COMPANY                MACHINE  FLAGS
COMP( 1984, mad1, 0,      0,      mad1,    mad1,  mad1_state, empty_init, "Mad Computers Inc.", "MAD-1", MACHINE_IMPERFECT_GRAPHICS )
