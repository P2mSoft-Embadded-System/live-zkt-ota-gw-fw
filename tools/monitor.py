#!/usr/bin/env python3
"""Log the device's serial output, optionally sending commands first.

  tools/monitor.py [-p PORT] [-t SECONDS] [-u REGEX] [--no-reset] [command ...]

  -t  stop after this many seconds (default 60)
  -u  stop early once a line matches this regex
  --no-reset  do not pulse EN when opening the port
Each command is sent as one line, e.g.:  tools/monitor.py "ssid MyNet" "pass secret" reboot
"""
import argparse
import glob
import re
import sys
import time

import serial

ap = argparse.ArgumentParser()
ports = sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*"))
ap.add_argument("-p", "--port", default=ports[0] if ports else "/dev/ttyUSB0")
ap.add_argument("-t", "--timeout", type=float, default=60)
ap.add_argument("-u", "--until")
ap.add_argument("--no-reset", action="store_true")
ap.add_argument("commands", nargs="*")
args = ap.parse_args()

ser = serial.Serial()
ser.port, ser.baudrate, ser.timeout = args.port, 115200, 0.2
ser.dtr = ser.rts = False  # keep the board out of reset / bootloader mode
ser.open()
if not args.no_reset:
    ser.rts = True  # pulse EN
    time.sleep(0.1)
    ser.rts = False

end = time.time() + args.timeout
pattern = re.compile(args.until) if args.until else None
pending = list(args.commands)
send_at = time.time() + 2.5 if pending else None  # let the board finish booting

while time.time() < end:
    if send_at and time.time() >= send_at:
        ser.write((pending.pop(0) + "\n").encode())
        send_at = time.time() + 0.5 if pending else None
    raw = ser.readline()
    if not raw:
        continue
    line = raw.decode(errors="replace").rstrip()
    print(line, flush=True)
    if pattern and not pending and pattern.search(line):
        sys.exit(0)
sys.exit(1 if pattern else 0)
