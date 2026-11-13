# MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
"""Usage: serlog.py <port> <file> <seconds>. Append everything a serial port prints to a file, with a PC timestamp per line. Reopens after a reset."""
import sys
import time

import serial

port, path, seconds = sys.argv[1], sys.argv[2], float(sys.argv[3])
end = time.time() + seconds
with open(path, "a", encoding="utf-8", errors="replace") as out:
    while time.time() < end:
        try:
            with serial.Serial(port, 115200, timeout=0.5) as s:
                # The USB-Serial/JTAG port resets the chip if DTR/RTS toggle: hold both low.
                s.dtr = False
                s.rts = False
                while time.time() < end:
                    line = s.readline()
                    if line:
                        out.write("%.3f %s\n" % (time.time(), line.decode("utf-8", "replace").rstrip()))
                        out.flush()
        except (serial.SerialException, OSError) as e:
            out.write("%.3f [serlog] %s\n" % (time.time(), e))
            out.flush()
            time.sleep(0.5)
