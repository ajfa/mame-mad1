#!/usr/bin/env bash
#
#  Run the MAD-1 under a MAME built with this driver.
#
#    ./run.sh                boot PC-DOS in a window
#    ./run.sh --check        headless: boot DOS, type DIR, verify the screen
#    ./run.sh --monitor      no disk, into the ROM's System Monitor
#
#  MAME=path/to/mad1  DISK=path/to/dos.img  ./run.sh
#
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MAME="${MAME:-$HERE/mame/mad1}"
DISK="${DISK:-$HERE/disks/dos330-360k.img}"
ROMS="${ROMS:-$HERE/roms}"
SECS="${SECS:-90}"

COMMON=(-rompath "$ROMS" -window -nomaximize -skip_gameinfo)

case "${1:-}" in
--check)
	rm -rf "$HERE/snap/mad1"
	# SDL opens the display even with -video none, so force it away for SSH;
	# do NOT redirect stdin from /dev/null, MAME then never posts the
	# autoboot keystrokes.  Enter is the two characters \n, MAME's own escape.
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
	"$MAME" mad1 "${COMMON[@]}" -video none -sound none -natural \
		-flop1 "$DISK" -autoboot_delay 55 -autoboot_command '\n\ndir\n' \
		-snapshot_directory "$HERE/snap" -str "$SECS" >"$HERE/run.log" 2>&1

	shot=$(ls -t "$HERE"/snap/mad1/*.png 2>/dev/null | head -1)
	if [ -z "$shot" ]; then
		tail -20 "$HERE/run.log"
		echo "no snapshot produced" >&2
		exit 1
	fi
	lit=$(python3 "$HERE/tools/checkscreen.py" "$shot")
	echo "screen: $shot ($lit lit pixels)"
	if [ "$lit" -lt 8000 ]; then
		echo "the directory listing is not on screen" >&2
		exit 1
	fi
	echo "PASS - PC-DOS booted and listed the disk"
	;;
--monitor)
	"$MAME" mad1 "${COMMON[@]}"
	;;
*)
	"$MAME" mad1 "${COMMON[@]}" -natural -flop1 "$DISK" "$@"
	;;
esac
