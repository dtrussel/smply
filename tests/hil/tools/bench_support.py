# SPDX-License-Identifier: Apache-2.0
"""What both bench supervisors need: the board, the console port, and a deadline.

`run_hil.py` and `crosscheck.py` drive the same bench and had begun to drift
apart on the parts where being wrong is expensive. The clearest case: the
baseline flasher exits **2** for "no probe, no board" and **1** for a real
failure (`firmware/flash_baseline.py`), and ADR-0015 turns on that distinction --
"unavailable" is never a pass and never a fail. `run_hil.py` mapped it
correctly; `crosscheck.py` mapped *every* unexpected code to "no bench", so a
crash in the flasher would have been reported as an absent bench. One copy of
that mapping, here, is the fix.

Nothing in this module knows what a case or a client *is*. It owns three things:

* **`Bench`** -- the ST-LINK reflash and the console capture, with the exit-code
  mapping and the "stop the logger before anything else opens the port" rule.
* **`run_step()`** -- a subprocess with a hard deadline that **never raises**.
  A caller that must retry cannot use a helper that throws past its loop, which
  is the defect `crosscheck.oracle_state()` had: its three attempts could not
  retry a timeout because the timeout escaped the loop.
* **`UartPort`** -- an arbiter, because only one process may hold the console
  port (tests/hil/README.md) and the cross-check needs it for the capture, for
  every oracle read and for a whole client arm.
"""
import os
import re
import subprocess
import sys
import threading
import time
from contextlib import contextmanager
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
FLASH = HERE / "firmware" / "flash_baseline.py"
UART_LOG = HERE / "tools" / "uart_log.py"
CUBE_CLI = Path(r"C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer"
                r"\bin\STM32_Programmer_CLI.exe")


def slug(text: str) -> str:
    """A directory name from a case or arm name: lowercase, dashes, bounded."""
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")[:60]


def child_environment() -> dict:
    """The environment every bench child gets.

    `PYTHONIOENCODING` is not decoration. `smpmgr` draws a progress spinner
    through `rich`, and when its stdout is a pipe rather than a console Python
    picks the ANSI code page -- cp1252 here -- which cannot encode the braille
    characters the spinner uses. The tool then dies with a `UnicodeEncodeError`
    in the middle of an upload, and the run reports a client failure that is
    entirely an artefact of being watched. The other two clients are not Python
    and ignore the variable.
    """
    return dict(os.environ, PYTHONIOENCODING="utf-8")


def run_step(cmd, log: Path, timeout: int, *, cwd=None, env=None) -> dict:
    """Runs \\p cmd to completion or to \\p timeout, and returns what happened.

    **Never raises**, deliberately. Every caller here is deciding a verdict, and
    a verdict function that has to catch an exception to notice a timeout gets
    the retry structure wrong -- which is exactly what happened before this
    existed. The record is the whole result: `timed_out` is a field, not a
    control-flow event.

    The output is drained on a daemon thread rather than by `capture_output`,
    because a child that fills the pipe while the parent waits on `wait()`
    deadlocks, and these children are chatty (a 134 KiB upload over UART prints
    per chunk). Same pattern as `run_hil.py`'s case runner, for the same reason.
    """
    started = time.monotonic()
    text = " ".join(str(c) for c in cmd)
    proc = subprocess.Popen([str(c) for c in cmd], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                            errors="replace", bufsize=1, cwd=cwd,
                            env=env if env is not None else child_environment())
    lines: list[str] = []

    def pump() -> None:
        for line in proc.stdout:
            lines.append(line)

    reader = threading.Thread(target=pump, daemon=True)
    reader.start()
    timed_out = False
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        proc.kill()
        proc.wait()
    reader.join(timeout=5)

    seconds = round(time.monotonic() - started, 1)
    output = "".join(lines)
    with log.open("a", encoding="utf-8") as out:
        out.write(f"$ {text}\n")
        out.write(f"-- TIMEOUT after {timeout}s --\n" if timed_out
                  else f"[exit {proc.returncode}, {seconds}s]\n")
        out.write(output + ("\n" if output and not output.endswith("\n") else ""))
    return {"cmd": text, "rc": proc.returncode, "timed_out": timed_out,
            "seconds": seconds, "stdout": output}


class Bench:
    """The board: reflash it, and capture its console.

    Constructed from a parsed argument namespace carrying `python`, `evidence`,
    `cli` and `uart` -- both supervisors name them the same, and passing the
    namespace keeps this module out of their argument definitions.
    """

    def __init__(self, args) -> None:
        self.args = args
        self.python = getattr(args, "python", None) or sys.executable

    def flash_baseline(self, log: Path, image: str = "a") -> str:
        """Reprograms MCUboot and one signed image, and says how it went.

        Returns `"ok"`, `"unavailable"` or `"failed"`. **The mapping is the
        point**: `flash_baseline.py` exits 2 only for "no probe, no board", and
        collapsing that into a failure -- or, worse, collapsing a failure into
        it -- is what ADR-0015 forbids. Anything unexpected is `"failed"`,
        because an unknown exit code from the recovery primitive is not evidence
        that the bench is absent.

        \\p image selects which of the two signed images lands in slot 0. The
        oracle self-test uses `"b"`; every other caller wants the default.
        """
        step = run_step([self.python, FLASH, "--evidence", self.args.evidence,
                         "--cli", self.args.cli, "--image", image], log, 300)
        if step["timed_out"]:
            return "failed"
        return {0: "ok", 2: "unavailable"}.get(step["rc"], "failed")

    def start_uart(self, out: Path):
        """Starts the console logger, or returns None when no port was given.

        The logger is stopped by closing its stdin, which is why it gets a pipe
        it never reads from.
        """
        if not self.args.uart:
            return None
        return subprocess.Popen([self.python, str(UART_LOG), "--port", self.args.uart,
                                 "--out", str(out)],
                                stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                stderr=subprocess.PIPE)

    @staticmethod
    def stop_uart(proc) -> None:
        if proc is None:
            return
        try:
            proc.stdin.close()
            proc.wait(timeout=10)
        except Exception:  # noqa: BLE001 -- best effort; the logger is not the test
            proc.kill()


class UartPort:
    """Hands the console port to one holder at a time.

    Only one process may open it (tests/hil/README.md), and the cross-check has
    three would-be holders: the console logger, every oracle read, and the whole
    UART client arm. `run_hil.py` never needed this because nothing there
    competes -- it holds the port for a group and no case touches it.

    Both context managers are re-entrant in the sense that matters: asking for
    what is already held is a no-op, so a `client()` inside a `client()` does not
    stop a logger that is not running.
    """

    def __init__(self, bench: Bench, log_dir: Path) -> None:
        self._bench = bench
        self._log_dir = log_dir
        self._logger = None
        self._base = None
        self._segment = 0
        self.segments: list[Path] = []

    def _next_segment(self) -> Path:
        """The next console-log file name.

        Numbered, because `uart_log.py` opens its output with `"w"` and so
        **truncates**: handing the restarted logger the name it used before
        would destroy everything captured up to the client that interrupted it.
        The gap between segments is where a client held the port, and the file
        names say so.
        """
        self._segment += 1
        name = (self._log_dir / f"{self._base}.uart.log" if self._segment == 1
                else self._log_dir / f"{self._base}.uart.{self._segment}.log")
        self.segments.append(name)
        return name

    @contextmanager
    def console(self, name: str):
        """Runs the console logger for the duration, in `<name>.uart[.N].log`."""
        if self._bench.args.uart is None or self._logger is not None:
            yield None
            return
        self._base = name
        self._segment = 0
        self._logger = self._bench.start_uart(self._next_segment())
        try:
            yield self.segments[0]
        finally:
            Bench.stop_uart(self._logger)
            self._logger = None

    @contextmanager
    def client(self):
        """Frees the port so another process may open it, then gives it back.

        A client that needs the port -- the oracle, or the UART arm -- must not
        merely hope the logger is elsewhere. Stopping and restarting the logger
        around it means the console log has a gap rather than the client having
        a mysterious "access denied".
        """
        held = self._logger is not None
        if held:
            Bench.stop_uart(self._logger)
            self._logger = None
        try:
            yield
        finally:
            if held:
                self._logger = self._bench.start_uart(self._next_segment())
