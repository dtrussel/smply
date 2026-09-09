# SPDX-License-Identifier: Apache-2.0
"""Record the host controller's HCI traffic while a BLE client runs.

Microsoft's Bluetooth Test Platform exposes the local controller's HCI traffic
over a TCP socket: `btvs.exe -Mode Wireshark -Remote on` listens on
`127.0.0.1:24352` and `tshark -i TCP@127.0.0.1:24352` records it
(tests/hil/README.md). **`btvs.exe` needs an elevated shell**; `tshark` attaching
to the socket does not, because a TCP client connection needs no privileges.

That asymmetry is the whole design here. Three situations, and the module reports
which one it was in rather than pretending the difference does not matter:

* **attach** -- something already has BTVS listening, started by a person in an
  elevated shell. This is the normal case for an unelevated session, and the only
  one it can reach.
* **spawn** -- this process is itself elevated and starts BTVS. Written for a
  commissioned self-hosted runner, which runs as a service and can. **Not
  verified**: the session that wrote it could not elevate, so this branch has
  never executed. Said plainly rather than left to be assumed.
* **unavailable** -- neither. ADR-0015: the capture is *reported* unavailable,
  never silently omitted, and a run without one is not a run that passed.

It never attempts `runas` or any other elevation prompt. A prompt nobody is
present to answer stalls the bench for as long as it takes someone to notice.

### A listening port is not a working capture

`build/capture-probe.pcapng`, recorded during P17a, is a **valid pcapng with a
correct interface name, the right encapsulation, and zero packets**. BTVS's
listener accepts the TCP connection and negotiates the link type without
elevation; what it will not do unelevated is deliver packets. So "the socket
connected" and "tshark started" are both true in the failing case, and the only
thing that discriminates is the **packet count**. Every caller must go through
`CaptureResult.status`, and a `status` of `empty` is not evidence of a quiet
radio -- it is the signature of this exact failure.
"""
import ctypes
import hashlib
import re
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

BTVS = Path(r"C:\BTP\v1.14.0\x86\btvs.exe")
TSHARK = Path(r"C:\Program Files\Wireshark\tshark.exe")
CAPINFOS = Path(r"C:\Program Files\Wireshark\capinfos.exe")
REMOTE_HOST, REMOTE_PORT = "127.0.0.1", 24352
INTERFACE = f"TCP@{REMOTE_HOST}:{REMOTE_PORT}"


def _listening(timeout: float = 0.5) -> bool:
    try:
        with socket.create_connection((REMOTE_HOST, REMOTE_PORT), timeout):
            return True
    except OSError:
        return False


def _elevated() -> bool:
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except (AttributeError, OSError):
        return False


def packet_count(capture: Path, capinfos: Path = CAPINFOS) -> int | None:
    """How many packets the file holds, or None if that cannot be established.

    `capinfos -c -M` is the authority; `tshark -r` counting lines is the
    fallback for a machine with tshark but no capinfos. None is not zero, and
    the caller must not treat it as such: "I could not count" and "there were
    none" are different findings.
    """
    if capinfos.exists():
        result = subprocess.run([str(capinfos), "-c", "-M", str(capture)],
                                capture_output=True, text=True, timeout=120)
        match = re.search(r"Number of packets\D+(\d+)", result.stdout)
        if match:
            return int(match.group(1))
    if TSHARK.exists():
        result = subprocess.run([str(TSHARK), "-r", str(capture), "-T", "fields",
                                 "-e", "frame.number"],
                                capture_output=True, text=True, timeout=300)
        if result.returncode == 0:
            return sum(1 for line in result.stdout.splitlines() if line.strip())
    return None


class HciCapture:
    """One capture, for the duration of one client arm."""

    def __init__(self, out: Path, *, mode: str = "auto", btvs: Path = BTVS,
                 tshark: Path = TSHARK, capinfos: Path = CAPINFOS,
                 duration: int = 3600) -> None:
        self.out = out
        self.mode = mode
        self.btvs, self.tshark, self.capinfos = btvs, tshark, capinfos
        self.duration = duration
        self.how = "unavailable"
        self.detail = ""
        self._btvs_proc = None
        self._tshark_proc = None
        self._started = 0.0

    def probe(self) -> tuple[str, str]:
        """Decides how this capture could happen, without starting anything."""
        if self.mode == "off":
            return "unavailable", "capture disabled by request"
        if not self.tshark.exists():
            return "unavailable", f"tshark not found at {self.tshark}"
        if self.mode in ("auto", "attach") and _listening():
            return "attach", f"btvs is already listening on {INTERFACE}"
        if self.mode == "attach":
            return "unavailable", (f"nothing is listening on {INTERFACE}; start "
                                   f'"{self.btvs}" -Mode Wireshark -Remote on '
                                   "in an elevated shell")
        if self.mode in ("auto", "spawn"):
            if not self.btvs.exists():
                return "unavailable", f"btvs not found at {self.btvs}"
            if _elevated():
                return "spawn", "this shell is elevated"
            return "unavailable", ("btvs is not running and this shell is not "
                                   "elevated; start it by hand, elevated")
        return "unavailable", f"unknown capture mode {self.mode!r}"

    def start(self) -> str:
        """Starts (or attaches to) the capture. Returns `how`."""
        self.how, self.detail = self.probe()
        if self.how == "unavailable":
            return self.how

        if self.how == "spawn":
            # UNVERIFIED PATH: see the module docstring. Written so a
            # commissioned runner needs no second code path, never executed.
            self._btvs_proc = subprocess.Popen(
                [str(self.btvs), "-Mode", "Wireshark", "-Remote", "on"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline and not _listening():
                time.sleep(0.5)
            if not _listening():
                self.how = "unavailable"
                self.detail = "btvs was started but never listened"
                return self.how

        self.out.parent.mkdir(parents=True, exist_ok=True)
        self._tshark_proc = subprocess.Popen(
            [str(self.tshark), "-i", INTERFACE, "-w", str(self.out), "-q",
             "-a", f"duration:{self.duration}", "-a", "filesize:200000"],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
            creationflags=getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0))
        # tshark needs a moment to open the interface and write the header; a
        # client that connects before then loses its first packets.
        time.sleep(2.0)
        self._started = time.monotonic()
        return self.how

    def stop(self) -> dict:
        """Stops the capture and reports what it holds.

        tshark on Windows does **not** stop when its stdin closes, unlike
        `uart_log.py`, so it is asked to stop with a console control event and
        then escalated. Escalation matters: a killed tshark can leave a file
        without its final block, and the packet count below is what notices.
        """
        seconds = round(time.monotonic() - self._started, 1) if self._started else 0.0
        if self._tshark_proc is not None:
            try:
                self._tshark_proc.send_signal(
                    getattr(signal, "CTRL_BREAK_EVENT", signal.SIGTERM))
                self._tshark_proc.wait(timeout=15)
            except Exception:  # noqa: BLE001 -- escalate rather than hang the run
                self._tshark_proc.terminate()
                try:
                    self._tshark_proc.wait(timeout=10)
                except Exception:  # noqa: BLE001
                    self._tshark_proc.kill()
            self._tshark_proc = None
        if self._btvs_proc is not None:
            self._btvs_proc.terminate()
            self._btvs_proc = None

        if self.how == "unavailable":
            return {"status": "unavailable", "how": self.how, "detail": self.detail,
                    "interface": INTERFACE, "packets": None}
        if not self.out.exists():
            return {"status": "unavailable", "how": self.how, "packets": None,
                    "detail": "tshark wrote no file", "interface": INTERFACE}

        raw = self.out.read_bytes()
        packets = packet_count(self.out, self.capinfos)
        if packets is None:
            status, detail = "unavailable", "the packet count could not be established"
        elif packets == 0:
            status, detail = "empty", (
                "the capture holds 0 packets: the BTVS link negotiated but "
                "delivered nothing, which is what an unelevated btvs looks like")
        else:
            status, detail = "ok", ""
        return {"status": status, "how": self.how, "detail": detail or self.detail,
                "interface": INTERFACE, "packets": packets, "seconds": seconds,
                "file": str(self.out), "bytes": len(raw),
                "sha256": hashlib.sha256(raw).hexdigest(),
                "btvs": str(self.btvs) if self.how == "spawn" else None}


def main() -> int:
    """A ten-second probe: is the capture path working at all?

    Worth its own entry point. Finding out that a capture is empty *after* a
    five-minute client arm wastes the arm; finding out in ten seconds does not.
    It cannot distinguish "the link is broken" from "nothing was talking", so it
    reports the count and lets the reader decide.
    """
    import argparse
    parser = argparse.ArgumentParser(description=main.__doc__)
    parser.add_argument("--out", type=Path, default=Path("build/hil-crosscheck/probe.pcapng"))
    parser.add_argument("--seconds", type=int, default=10)
    parser.add_argument("--mode", default="auto", choices=["auto", "attach", "spawn", "off"])
    parser.add_argument("--btvs", type=Path, default=BTVS)
    parser.add_argument("--tshark", type=Path, default=TSHARK)
    parser.add_argument("--capinfos", type=Path, default=CAPINFOS)
    args = parser.parse_args()

    capture = HciCapture(args.out, mode=args.mode, btvs=args.btvs, tshark=args.tshark,
                         capinfos=args.capinfos, duration=args.seconds + 30)
    how, detail = capture.probe()
    print(f"probe: {how} -- {detail}")
    if how == "unavailable":
        return 2
    capture.start()
    print(f"capturing for {args.seconds}s ...", flush=True)
    time.sleep(args.seconds)
    result = capture.stop()
    print(f"status={result['status']} packets={result['packets']} "
          f"bytes={result.get('bytes')} how={result['how']}")
    if result["detail"]:
        print(f"  {result['detail']}")
    return {"ok": 0, "empty": 1}.get(result["status"], 2)


if __name__ == "__main__":
    sys.exit(main())
