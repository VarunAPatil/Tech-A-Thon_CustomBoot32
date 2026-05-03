#!/usr/bin/env python3
"""
serial_logger.py  —  Captures [LOG] lines from ESP32 serial and saves to a .txt file.

Usage:
    python3 serial_logger.py --port /dev/ttyUSB0 --baud 115200 --out fan_max_log.txt

The ESP32 prints lines like:
    [LOG] t=1234 ms  TEMP=24.50 C

This script captures those lines and writes:
    2026-02-27 22:10:05  |  t=1234 ms  |  TEMP=24.50 C
"""

import serial
import argparse
import re
from datetime import datetime

LOG_PATTERN = re.compile(r'\[LOG\]\s+(.*)')

def main():
    parser = argparse.ArgumentParser(description="ESP32 temperature serial logger")
    parser.add_argument('--port', default='/dev/ttyUSB0', help='Serial port (default: /dev/ttyUSB0)')
    parser.add_argument('--baud', type=int, default=115200, help='Baud rate (default: 115200)')
    parser.add_argument('--out',  default='temp_log.txt', help='Output file name (default: temp_log.txt)')
    args = parser.parse_args()

    print(f"Connecting to {args.port} @ {args.baud} baud ...")
    print(f"Logging to   : {args.out}")
    print("Press Ctrl+C to stop.\n")

    with serial.Serial(args.port, args.baud, timeout=2) as ser, \
         open(args.out, 'a') as f:

        # Write header if file is new/empty
        f.seek(0, 2)
        if f.tell() == 0:
            f.write("wall_time                |  esp_elapsed    |  temperature\n")
            f.write("-" * 60 + "\n")
        f.flush()

        while True:
            try:
                raw = ser.readline().decode('utf-8', errors='replace').strip()
            except KeyboardInterrupt:
                print("\nStopped by user.")
                break

            if not raw:
                continue

            # Print everything to console so you can still see boot messages etc.
            print(raw)

            match = LOG_PATTERN.search(raw)
            if match:
                payload = match.group(1)   # e.g. "t=1234 ms  TEMP=24.50 C"
                # Parse elapsed and temp
                t_match    = re.search(r't=(\d+)\s*ms', payload)
                temp_match = re.search(r'TEMP=([^\s]+)', payload)

                elapsed_str = t_match.group(0)    if t_match    else "t=? ms"
                temp_str    = temp_match.group(0) if temp_match else "TEMP=?"

                wall = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
                line = f"{wall}  |  {elapsed_str:<16}|  {temp_str}\n"
                f.write(line)
                f.flush()

if __name__ == '__main__':
    main()
