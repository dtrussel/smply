# SPDX-License-Identifier: Apache-2.0
"""Capture the peer's console UART to a file, one timestamped line per line.

The NUCLEO-WB55RG routes USART1 to the ST-LINK virtual COM port; MCUboot and
smp_svr both log there. Every HIL case runs with one of these alongside it so a
failure can be read from the device's side, not only the client's.

Usage: uart_log.py --port COM4 --out case.uart.log [--baud 115200] [--seconds N]
Stops on --seconds, on Ctrl-C, or when its stdin closes (the supervisor's way of
asking it to stop). Exit 2 if the port cannot be opened -- a bench problem, not a
test result.
"""
import argparse
import sys
import threading
import time
from pathlib import Path

import serial  # pyserial


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=None,
                        help="stop after this long (default: run until stdin closes)")
    parser.add_argument("--echo", action="store_true", help="also print lines to stdout")
    args = parser.parse_args()

    try:
        port = serial.Serial(args.port, args.baud, timeout=0.2)
    except serial.SerialException as error:
        print(f"uart_log: cannot open {args.port}: {error}", file=sys.stderr)
        return 2

    stop = threading.Event()

    def watch_stdin() -> None:
        try:
            sys.stdin.read()
        except Exception:  # noqa: BLE001 -- a closed or absent stdin means stop
            pass
        stop.set()

    if args.seconds is None:
        threading.Thread(target=watch_stdin, daemon=True).start()

    started = time.monotonic()
    deadline = None if args.seconds is None else started + args.seconds
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", encoding="utf-8", errors="replace") as out:
        out.write(f"# uart_log {args.port} {args.baud} start={time.strftime('%Y-%m-%dT%H:%M:%S')}\n")
        pending = b""
        try:
            while not stop.is_set() and (deadline is None or time.monotonic() < deadline):
                pending += port.read(4096)
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    text = line.decode("utf-8", errors="replace").rstrip("\r")
                    stamp = f"{time.monotonic() - started:10.3f}"
                    out.write(f"{stamp} {text}\n")
                    out.flush()
                    if args.echo:
                        print(f"{stamp} {text}")
        except KeyboardInterrupt:
            pass
        finally:
            if pending:
                out.write(f"{time.monotonic() - started:10.3f} {pending.decode('utf-8', 'replace')}\n")
            port.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
