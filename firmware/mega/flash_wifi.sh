#!/bin/bash
# Build the Mega sketch and flash it over WiFi through the onboard ESP8266. No USB, no reset button.
#   ./flash_wifi.sh [esp-ip-or-name]        default: pss270.local
# Needs arduino-cli with the Arduino AVR core installed (it brings its own avrdude).
# The ESP receives the connection on TCP 2323, tells the Mega to jump into its bootloader and
# passes the bytes through. DIP 1+2 ON and the slide switch on RXD0/TXD0, see the README.
set -e
HOST="${1:-pss270.local}"
cd "$(dirname "$0")"
FQBN="arduino:avr:mega:cpu=atmega2560"
arduino-cli compile --fqbn "$FQBN" --output-dir build pss270_mega | tail -2

DATA="$(arduino-cli config get directories.data)"
AVRDIR="$(ls -d "$DATA"/packages/arduino/tools/avrdude/* | sort | tail -1)"
AVRDUDE="$AVRDIR/bin/avrdude"; [ -x "$AVRDUDE" ] || AVRDUDE="$AVRDIR/bin/avrdude.exe"
HEX="$PWD/build/pss270_mega.ino.hex"; command -v cygpath >/dev/null && HEX="$(cygpath -m "$HEX")"

"$AVRDUDE" -C "$AVRDIR/etc/avrdude.conf" -p m2560 -c stk500v2 -P "net:$HOST:2323" -b 115200 -D \
  -U "flash:w:$HEX:i" 2>&1 | grep -iE "verified|error" | tail -3
