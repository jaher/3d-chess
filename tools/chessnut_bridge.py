#!/usr/bin/env python3
"""Chessnut Move BLE bridge.

Long-running helper subprocess that the desktop chess app spawns
when the Chessnut Move toggle is on. Speaks a tiny line-based
protocol over stdin/stdout (mirrors the Stockfish subprocess
pattern in ai_player.cpp).

Stdin commands (one per line):
  INIT [target]            connect to a Chessnut device. Without an
                           argument, the helper first tries the
                           cached MAC address from
                           ~/.cache/chessnut_bridge_address (or the
                           CHESS_CHESSNUT_ADDRESS env var) and only
                           falls back to a name scan when neither is
                           set. Explicit forms:
                             INIT 00:1B:10:51:9D:63  → by MAC
                             INIT chessnut          → by name (substring)
  SCAN                     list every nearby BLE peripheral that
                           advertises a name (for diagnosing the
                           exact name your board uses).
  FEN <fen>                send the current target position. Soft mode
                           (board only re-plans if state changed).
  FEN_FORCE <fen>          send target position. Hard mode (always
                           re-plans from current sensor state) — use
                           on game start / puzzle load.
  LED <16hex>              16 hex chars = 8 bytes = 64-bit LED bitmask
                           (bit per square; row-major from h1).
  PING                     replies PONG (liveness check).
  QUIT                     disconnect and exit.

Stdout responses (one per line):
  READY                    process started, waiting for INIT.
  CONNECTED <name>         connected to a board.
  DISCONNECTED             connection lost.
  ERROR <msg>              recoverable failure for the last command.
  FATAL <msg>              non-recoverable; bridge exits.
  NOTIFY <uuid> <hex>      raw notification (diagnostic only).
  ACK <cmd>                successful completion of the named command.

Protocol details from third_party reverse-engineering — see
PROTOCOL.md in the chessnutapp folder for the full spec.
"""

from __future__ import annotations  # defer type-annotation evaluation

import asyncio
import os
import re
import sys
from pathlib import Path
from typing import Optional


# Last-connected MAC is cached here so subsequent INITs go straight
# to the right peripheral instead of doing an 8 s discovery scan.
# Plain text, one line. Removed when QUIT fires after a clean
# connect so the cache stays accurate across reboots.
ADDRESS_CACHE = Path(os.path.expanduser(
    "~/.cache/chessnut_bridge_address"))


def is_mac_address(s: str) -> bool:
    return bool(re.fullmatch(
        r"[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}", s))


def load_cached_address() -> Optional[str]:
    if env := os.environ.get("CHESS_CHESSNUT_ADDRESS"):
        return env.strip() or None
    try:
        s = ADDRESS_CACHE.read_text().strip()
        return s if is_mac_address(s) else None
    except FileNotFoundError:
        return None
    except OSError:
        return None


def save_cached_address(addr: str) -> None:
    try:
        ADDRESS_CACHE.parent.mkdir(parents=True, exist_ok=True)
        ADDRESS_CACHE.write_text(addr + "\n")
    except OSError:
        pass  # cache failure shouldn't break the connection


# ---------------------------------------------------------------------------
# GATT UUIDs — see PROTOCOL.md
# ---------------------------------------------------------------------------
SUFFIX = "-2877-41c3-b46e-cf057c562023"

UUID_BOARD_NOTIFY_A = f"1b7e8261{SUFFIX}"  # Move-only
UUID_BOARD_NOTIFY_B = f"1b7e8262{SUFFIX}"  # Air-compatible board state
UUID_CMD_WRITE_A    = f"1b7e8271{SUFFIX}"  # Move-only command channel
UUID_CMD_WRITE_B    = f"1b7e8272{SUFFIX}"  # Air-compatible cmd write
UUID_CMD_NOTIFY_A   = f"1b7e8271{SUFFIX}"  # Move-only (bidirectional)
UUID_CMD_NOTIFY_B   = f"1b7e8273{SUFFIX}"  # Air-compatible cmd response

NOTIFY_UUIDS = [
    UUID_BOARD_NOTIFY_A,
    UUID_BOARD_NOTIFY_B,
    UUID_CMD_NOTIFY_A,
    UUID_CMD_NOTIFY_B,
]
WRITE_UUID = UUID_CMD_WRITE_B


# ---------------------------------------------------------------------------
# Piece encoding — must match ChessnutService.PIECEMAP byte-for-byte.
# Lowercase = black, uppercase = white. Space = empty.
# ---------------------------------------------------------------------------
PIECE_NIBBLE = {
    " ": 0, "q": 1, "k": 2, "b": 3, "p": 4, "n": 5,
    "R": 6, "P": 7, "r": 8, "B": 9, "N": 10, "Q": 11, "K": 12,
}


def fen_to_board_bytes(fen: str) -> bytes:
    """Encode the piece-placement portion of a FEN into the 32-byte
    4-bits-per-square format the Move firmware expects.

    Layout (decoded from ChessnutService.java:330-350):
      - 8 rows, 4 bytes per row.
      - Within a row, columns are paired (2 squares per byte).
      - The pair index `i2` runs 0..3 but is stored at offset
        `(3 - i2)` — i.e. the h-pair lands at offset 0, a-pair at
        offset 3.
      - High nibble = first square of the pair, low nibble = second.
    """
    placement = fen.split()[0]
    rows = placement.split("/")
    if len(rows) != 8:
        raise ValueError(f"FEN has {len(rows)} ranks, expected 8: {fen!r}")

    # Expand digits into spaces so each row is exactly 8 chars (a..h).
    expanded = []
    for r in rows:
        line = ""
        for c in r:
            if c.isdigit():
                line += " " * int(c)
            else:
                line += c
        if len(line) != 8:
            raise ValueError(f"row {r!r} expanded to {len(line)} squares")
        expanded.append(line)

    # FEN row 0 = rank 8 = top of board. The Move firmware appears to
    # iterate rows 0..7 the same way — see the java loop. Keep the
    # FEN order; the column-pair-reverse handles file orientation.
    out = bytearray(32)
    for i in range(8):
        row = expanded[i]
        for i2 in range(4):
            i3 = i2 * 2
            hi = PIECE_NIBBLE.get(row[i3], 0)
            lo = PIECE_NIBBLE.get(row[i3 + 1], 0)
            out[(i * 4) + (3 - i2)] = (hi << 4) | lo
    return bytes(out)


def build_set_move_board(fen: str, force: bool) -> bytes:
    """Build the full 35-byte 0x42 setMoveBoard frame.

    [0]   0x42                  opcode
    [1]   0x21 (=33)            payload length
    [2..33] 32 bytes             board state
    [34]  0 if force else 1     force flag (0 = always replan)
    """
    board = fen_to_board_bytes(fen)
    flag  = 0 if force else 1
    return bytes([0x42, 0x21]) + board + bytes([flag])


def build_led(bitmask_hex: str) -> bytes:
    """Build the 0x0A length-8 LED command from a 16-hex-char string."""
    if len(bitmask_hex) != 16:
        raise ValueError(f"LED bitmask must be 16 hex chars, got {len(bitmask_hex)}")
    payload = bytes.fromhex(bitmask_hex)
    return bytes([0x0A, 0x08]) + payload


def build_init_handshake() -> list[bytes]:
    """The two opening writes the Android app sends after connect.
    See ChessnutBLEDevice.java:342, 350."""
    return [
        bytes([0x0B, 0x04, 0x03, 0xE8, 0x00, 0xC8]),
        bytes([0x27, 0x01, 0x00]),
    ]


# ---------------------------------------------------------------------------
# Bridge state machine
# ---------------------------------------------------------------------------
class Bridge:
    def __init__(self) -> None:
        self.client: Optional[BleakClient] = None
        self.connected_name: Optional[str] = None

    async def stdout(self, line: str) -> None:
        sys.stdout.write(line + "\n")
        sys.stdout.flush()

    async def find_device(self, name_match: str = "chessnut"):
        """Scan for a Chessnut device. `name_match` is matched as a
        case-insensitive substring of the advertising name — picks
        up "Chessnut Move", "Chessnut Move 1234", "Chessnut Air",
        etc. Returns the BLEDevice or None on timeout."""
        needle = name_match.lower()

        def matches(dev, adv):
            n = (dev.name or
                 (adv.local_name if adv is not None else None) or "")
            return needle in n.lower()

        # 8 s window — long enough to wake a sleeping board, short
        # enough that a cold-start with no board nearby doesn't hang.
        return await BleakScanner.find_device_by_filter(
            matches, timeout=8.0)

    async def scan_dump(self) -> None:
        """List every nearby BLE peripheral with a name. Tags each
        device with its advertised service UUIDs so the user can
        spot the Chessnut family (1b7e82..-2877-...) regardless of
        what local name the firmware happens to advertise."""
        await self.stdout("scanning 5 s for advertising peripherals…")
        seen = {}  # addr -> (name, [service-uuids])

        def cb(dev, adv):
            n = (dev.name or
                 (adv.local_name if adv is not None else None) or "")
            uuids = list(adv.service_uuids) if adv is not None else []
            if n or uuids:
                seen[dev.address] = (n, uuids)

        scanner = BleakScanner(detection_callback=cb)
        await scanner.start()
        await asyncio.sleep(5.0)
        await scanner.stop()

        if seen:
            chessnut_marker = "2877-41c3-b46e-cf057c562023"
            for addr, (name, uuids) in seen.items():
                tag = ""
                for u in uuids:
                    if chessnut_marker in u.lower():
                        tag = " [CHESSNUT]"
                        break
                # The C++ picker parses "DEVICE <addr> <name>" and
                # ignores anything past the first space-after-name,
                # so the [CHESSNUT] / svc=… tags ride along as the
                # name's tail without confusing the parser.
                await self.stdout(
                    f"DEVICE {addr} {name or '(no name)'}{tag}")
        else:
            await self.stdout("(no devices visible)")
        await self.stdout("SCAN_COMPLETE")

    def on_disconnect(self, _client) -> None:
        # Schedule the report on the running loop — bleak fires this
        # synchronously from its disconnection handler.
        loop = asyncio.get_event_loop()
        loop.call_soon_threadsafe(asyncio.create_task,
                                   self.stdout("DISCONNECTED"))

    async def on_notify(self, sender, data: bytearray) -> None:
        # Diagnostic only — the C++ side currently ignores incoming
        # notifications. Useful when capturing protocol behavior.
        sender_uuid = getattr(sender, "uuid", str(sender))
        await self.stdout(
            f"NOTIFY {sender_uuid} {bytes(data).hex()}")

    async def init(self, target: str) -> None:
        if self.client is not None and self.client.is_connected:
            await self.stdout(f"CONNECTED {self.connected_name}")
            return

        # Resolve the target, in order:
        #  1) explicit MAC arg → fast by-address scan (1 s).
        #  2) explicit name substring arg → name-substring scan (8 s).
        #  3) no arg + cached MAC / env override → by-address (1 s).
        #     If that fails, fall back to a default substring scan
        #     so a board that moved or rebooted still gets found.
        device = None
        if target:
            if is_mac_address(target):
                device = await BleakScanner.find_device_by_address(
                    target, timeout=4.0)
            else:
                device = await self.find_device(target)
        else:
            cached = load_cached_address()
            if cached:
                device = await BleakScanner.find_device_by_address(
                    cached, timeout=4.0)
            if device is None:
                device = await self.find_device("chessnut")

        if device is None:
            await self.stdout(
                f"ERROR no Chessnut device found"
                f"{(' for target ' + repr(target)) if target else ''}"
                f" — try SCAN to list nearby peripherals, or INIT "
                f"<MAC|name-substring> with a specific target")
            return
        self.connected_name = device.name or "Chessnut Move"
        try:
            client = BleakClient(device, disconnected_callback=self.on_disconnect)
            await client.connect()
        except Exception as e:  # noqa: BLE001 — surface anything bleak throws
            await self.stdout(f"ERROR connect failed: {e}")
            return
        self.client = client

        # Subscribe to all notify channels so we get protocol traces.
        for uuid in NOTIFY_UUIDS:
            try:
                await client.start_notify(uuid, self.on_notify)
            except Exception as e:  # noqa: BLE001
                # Not all devices expose all four — Air won't have 8261/8271.
                await self.stdout(f"NOTIFY skip {uuid}: {e}")

        # Send the two-frame Move handshake.
        for frame in build_init_handshake():
            try:
                await client.write_gatt_char(WRITE_UUID, frame, response=False)
            except Exception as e:  # noqa: BLE001
                await self.stdout(f"ERROR handshake write failed: {e}")
                return

        # Cache the MAC for next time so subsequent INITs skip the scan.
        save_cached_address(device.address)
        await self.stdout(f"CONNECTED {self.connected_name}")

    async def write_frame(self, frame: bytes, ack_label: str) -> None:
        if self.client is None or not self.client.is_connected:
            await self.stdout(f"ERROR not connected")
            return
        try:
            await self.client.write_gatt_char(
                WRITE_UUID, frame, response=False)
        except Exception as e:  # noqa: BLE001
            await self.stdout(f"ERROR write failed: {e}")
            return
        await self.stdout(f"ACK {ack_label}")

    async def quit(self) -> None:
        if self.client is not None and self.client.is_connected:
            try:
                await self.client.disconnect()
            except Exception:
                pass
        self.client = None


async def main() -> None:
    # bleak is only imported when actually running — unit tests of
    # the encoder can `import chessnut_bridge` without it.
    global BleakClient, BleakScanner  # type: ignore[global-statement]
    try:
        from bleak import BleakClient, BleakScanner  # noqa: F401
    except ImportError:
        sys.stdout.write(
            "FATAL bleak not installed — run 'pip install --user bleak'\n")
        sys.stdout.flush()
        return

    bridge = Bridge()
    await bridge.stdout("READY")

    # Read stdin lines on the asyncio loop without blocking. Wraps
    # the blocking readline() in an executor so we can also process
    # asynchronous BLE events (notifications, disconnects).
    loop = asyncio.get_running_loop()
    while True:
        line = await loop.run_in_executor(None, sys.stdin.readline)
        if not line:
            await bridge.quit()
            return
        line = line.rstrip("\r\n")
        if not line:
            continue

        cmd, _, arg = line.partition(" ")
        cmd = cmd.upper()
        try:
            if cmd == "PING":
                await bridge.stdout("PONG")
            elif cmd == "INIT":
                # Empty arg → init() consults the cache + falls
                # back to a substring scan. Don't override here.
                await bridge.init(arg.strip())
            elif cmd == "SCAN":
                await bridge.scan_dump()
            elif cmd == "FEN":
                frame = build_set_move_board(arg.strip(), force=False)
                await bridge.write_frame(frame, "FEN")
            elif cmd == "FEN_FORCE":
                frame = build_set_move_board(arg.strip(), force=True)
                await bridge.write_frame(frame, "FEN_FORCE")
            elif cmd == "LED":
                frame = build_led(arg.strip())
                await bridge.write_frame(frame, "LED")
            elif cmd == "QUIT":
                await bridge.quit()
                return
            else:
                await bridge.stdout(f"ERROR unknown command {cmd!r}")
        except ValueError as e:
            await bridge.stdout(f"ERROR {e}")
        except Exception as e:  # noqa: BLE001
            await bridge.stdout(f"ERROR {e}")


if __name__ == "__main__":
    # Unbuffered output so the C++ parent sees lines immediately.
    os.environ.setdefault("PYTHONUNBUFFERED", "1")
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
