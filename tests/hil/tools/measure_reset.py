# SPDX-License-Identifier: Apache-2.0
"""Measure how long the peer is unreachable after an SMP reset.

`support/dfu_app/reconnect_policy.hpp` ships defaults that were chosen without
hardware (P16). This is the stopwatch that replaces the guess: for each of N
iterations it connects, sends an OS-group reset, and records -- relative to the
moment the device accepted the reset --

  t_disc   when Windows reported the link gone (a silent reset is learnt by
           supervision timeout, so this is the central's number, not the device's),
  t_adv    when the device's first advertisement was seen again (the device's
           real unavailability window),
  t_conn   when a GATT connection succeeded again,
  t_ready  when the SMP characteristic answered an echo (usable, not just connected).

It is a bench instrument, not a protocol reference: the third-party `smp`/`bleak`
packages are used only to put a reset on the air and listen (ADR-0015). The
numbers it prints feed ReconnectSettings and the P17 outcome; nothing in the
library is derived from them without a protocol-notes entry.

Usage (from the bench venv):
  python measure_reset.py --address AA:BB:CC:DD:EE:FF --iterations 20 --out reset-plain.json
Only plain resets are measured here. Swap and revert resets happen inside the HIL
cases, which log the same four instants themselves.
"""
import argparse
import asyncio
import json
import statistics
import sys
import time
from pathlib import Path

from bleak import BleakClient, BleakScanner
from smp import header as smp_header
from smp.os_management import EchoWriteRequest, EchoWriteResponse, ResetWriteRequest
from smpclient.transport.ble import SMP_CHARACTERISTIC_UUID, SMP_SERVICE_UUID


class Notifications:
    """Collects SMP notifications into a queue, one whole response per item."""

    def __init__(self) -> None:
        self.queue: asyncio.Queue[bytes] = asyncio.Queue()
        self._pending = b""

    def __call__(self, _sender, data: bytearray) -> None:
        # Responses larger than one notification are reassembled from the SMP
        # header's length (protocol-notes section 8): no extra framing on BLE.
        self._pending += bytes(data)
        while len(self._pending) >= smp_header.Header.SIZE:
            hdr = smp_header.Header.loads(self._pending[: smp_header.Header.SIZE])
            total = smp_header.Header.SIZE + hdr.length
            if len(self._pending) < total:
                return
            self.queue.put_nowait(self._pending[:total])
            self._pending = self._pending[total:]


async def wait_for_advertisement(address: str, timeout: float):
    """The monotonic time of the first advertisement from `address`, and the device.

    It returns the `BLEDevice` as well as the instant, and the caller connects to
    *that* rather than to the address string. The first version of this tool threw
    it away, and a `BleakClient(address)` then had to find the peer itself -- which
    on WinRT means another scan, started just as this one stopped. Two scanners
    over one radio cost 13 of 20 iterations to "device not found", so most of the
    sample was measuring the tool. `(None, None)` if nothing was seen in time.
    """
    seen: asyncio.Future[tuple[float, object]] = asyncio.get_running_loop().create_future()

    def on_detect(device, _adv) -> None:
        if device.address.upper() == address.upper() and not seen.done():
            seen.set_result((time.monotonic(), device))

    scanner = BleakScanner(detection_callback=on_detect, service_uuids=[str(SMP_SERVICE_UUID)])
    await scanner.start()
    try:
        return await asyncio.wait_for(seen, timeout)
    except asyncio.TimeoutError:
        return None, None
    finally:
        await scanner.stop()


async def find_device(address: str, timeout: float):
    """Scans for `address` and returns the `BLEDevice`, or `None`.

    Needed before the *first* connect of an iteration for the same reason
    `wait_for_advertisement` hands its device back: on WinRT a `BleakClient`
    built from an address string has to resolve it, which means a scan of its
    own, and a cold OS cache then answers "device not found" however long the
    connect timeout is. Scanning once here and connecting to the object is one
    scan instead of one per retry.
    """
    return await BleakScanner.find_device_by_address(
        address, timeout=timeout, service_uuids=[str(SMP_SERVICE_UUID)])


async def connect_until(target, deadline: float, disconnected) -> tuple[BleakClient, float]:
    """Retries connecting until it works or the deadline passes.

    `target` is a `BLEDevice` when one is in hand -- which skips the address
    resolution that would otherwise start a scan of its own; see
    `wait_for_advertisement`. A plain address string still works and is what the
    first connect of an iteration uses.
    """
    attempt = 0
    while True:
        attempt += 1
        client = BleakClient(target, services=[str(SMP_SERVICE_UUID)], timeout=10.0,
                             disconnected_callback=disconnected)
        try:
            await client.connect()
            return client, time.monotonic()
        except Exception as error:  # noqa: BLE001 -- every failure here is "not yet"
            if time.monotonic() > deadline:
                raise TimeoutError(f"no connection after {attempt} attempts: {error}") from error
            await asyncio.sleep(0.2)


async def echo_round_trip(client: BleakClient, notes: Notifications, timeout: float) -> float:
    request = EchoWriteRequest(d="p17")
    await client.write_gatt_char(SMP_CHARACTERISTIC_UUID, request.BYTES, response=False)
    raw = await asyncio.wait_for(notes.queue.get(), timeout)
    EchoWriteResponse.loads(raw)  # raises if it is not the echo
    return time.monotonic()


async def one_iteration(address: str, timeouts: dict) -> dict:
    loop = asyncio.get_running_loop()
    disconnected: asyncio.Future[float] = loop.create_future()

    def on_disconnect(_client) -> None:
        if not disconnected.done():
            disconnected.set_result(time.monotonic())

    found = await find_device(address, timeouts["advertise"])
    if found is None:
        raise TimeoutError(f"no advertisement from {address} in {timeouts['advertise']}s")
    client, _ = await connect_until(found, time.monotonic() + timeouts["connect"], on_disconnect)
    notes = Notifications()
    try:
        await client.start_notify(SMP_CHARACTERISTIC_UUID, notes)
        await echo_round_trip(client, notes, timeouts["echo"])

        # Listen for the device's return *before* resetting it. The first run of
        # this tool started scanning only after Windows reported the disconnect,
        # and that report arrives ~9.8 s after a silent reset (the peer reboots
        # without a link-layer terminate, so the central learns by supervision
        # timeout) -- long after the device was already advertising again. A
        # connected device does not advertise, so anything seen after the reset
        # request is post-reset.
        advertising = asyncio.ensure_future(
            wait_for_advertisement(address, timeouts["advertise"] + timeouts["disconnect"]))

        # The reset. Acceptance is the response arriving (design.md section 5:
        # reset is acceptance, not completion).
        await client.write_gatt_char(SMP_CHARACTERISTIC_UUID, ResetWriteRequest().BYTES,
                                     response=False)
        raw = await asyncio.wait_for(notes.queue.get(), timeouts["echo"])
        t0 = time.monotonic()
        hdr = smp_header.Header.loads(raw[: smp_header.Header.SIZE])
        if hdr.group_id != smp_header.GroupId.OS_MANAGEMENT:
            raise RuntimeError(f"unexpected response to reset: group {hdr.group_id}")

        # Two independent observations, not two steps. The device advertises
        # again long before Windows reports the link gone -- a silent reset is
        # learnt by supervision timeout, ~9.8 s -- so awaiting them in sequence
        # let a missing disconnect report throw away a perfectly good
        # advertisement, and the row was lost rather than one of its cells.
        # Gathered, so each instant is recorded or absent on its own.
        reported = asyncio.wait_for(disconnected, timeouts["disconnect"])
        seen_disc, seen_adv = await asyncio.gather(reported, advertising,
                                                   return_exceptions=True)
    finally:
        try:
            await client.disconnect()
        except Exception:  # noqa: BLE001 -- it is gone already, which is the point
            pass

    t_disc = None if isinstance(seen_disc, BaseException) else seen_disc
    if isinstance(seen_adv, BaseException):
        seen_adv = (None, None)
    t_adv, device = seen_adv

    disconnected2: asyncio.Future[float] = loop.create_future()
    client, t_conn = await connect_until(
        device if device is not None else address, time.monotonic() + timeouts["connect"],
        lambda _c: disconnected2.done() or disconnected2.set_result(time.monotonic()))
    notes = Notifications()
    try:
        await client.start_notify(SMP_CHARACTERISTIC_UUID, notes)
        t_ready = await echo_round_trip(client, notes, timeouts["echo"])
    finally:
        await client.disconnect()

    return {
        "disconnect_ms": None if t_disc is None else round((t_disc - t0) * 1000),
        "advertise_ms": None if t_adv is None else round((t_adv - t0) * 1000),
        "connect_ms": round((t_conn - t0) * 1000),
        "ready_ms": round((t_ready - t0) * 1000),
    }


def summarise(rows: list[dict], key: str) -> dict:
    values = [r[key] for r in rows if r.get(key) is not None]
    if not values:
        return {"n": 0}
    values.sort()
    return {"n": len(values), "min": values[0], "median": statistics.median(values),
            "p90": values[min(len(values) - 1, int(0.9 * len(values)))], "max": values[-1]}


async def main_async(args) -> int:
    timeouts = {"connect": args.connect_timeout, "echo": 5.0, "disconnect": 20.0,
                "advertise": args.connect_timeout}
    rows: list[dict] = []
    for i in range(args.iterations):
        try:
            row = await one_iteration(args.address, timeouts)
        except Exception as error:  # noqa: BLE001 -- recorded, then the run goes on
            row = {"error": f"{type(error).__name__}: {error}"}
        row["iteration"] = i
        rows.append(row)
        print(json.dumps(row), flush=True)
        await asyncio.sleep(args.settle)

    result = {
        "address": args.address, "iterations": args.iterations, "kind": "plain",
        "rows": rows,
        "summary": {k: summarise(rows, k) for k in
                    ("disconnect_ms", "advertise_ms", "connect_ms", "ready_ms")},
        "errors": sum(1 for r in rows if "error" in r),
    }
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result["summary"], indent=2))
    return 0 if result["errors"] == 0 else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--address", required=True, help="the peer's Bluetooth address")
    parser.add_argument("--iterations", type=int, default=20)
    parser.add_argument("--connect-timeout", type=float, default=30.0,
                        help="give up waiting for the device after this many seconds")
    parser.add_argument("--settle", type=float, default=1.0, help="pause between iterations")
    parser.add_argument("--out", type=Path, default=None, help="write the JSON result here")
    args = parser.parse_args()
    return asyncio.run(main_async(args))


if __name__ == "__main__":
    sys.exit(main())
