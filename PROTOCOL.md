# Chessnut Move BLE protocol — RE notes (2026-04-30)

Reverse-engineered from the official Android app (`Chessnut Android_26.0.2026031200`)
via `jadx` decompile. Cross-checked against the documented Chessnut Air
protocol (community-RE'd in `rmarabini/chessnutair`,
`staubsauger/ChessnutPy`, `ecrucru/chessnut-connector`).

**TL;DR:** Move is a strict superset of Air. Same GATT service family, same
piece encoding, same board-state notify format. Move adds a second pair of
characteristics (`8271` / `8273`) for motor-, WiFi-, and firmware-specific
commands.

## GATT characteristics

All UUIDs share the suffix `2877-41c3-b46e-cf057c562023`. Listing them by
their 16-bit short:

| Short  | Direction        | Purpose                         | Air equivalent |
|--------|------------------|---------------------------------|----------------|
| `8262` | board → app  (notify) | Board-state stream (FEN-derived). Same 32-byte 4-bits-per-square format as Air. | ✓ same |
| `8272` | app → board  (write)  | Generic command channel.        | ✓ same |
| `8273` | board → app  (notify) | Command-response channel.       | ✓ same |
| `8261` | board → app  (notify) | **Move-only** — second board/state channel. | ✗ new |
| `8271` | both         (write + notify) | **Move-only** — motor + Wi-Fi + firmware command channel. | ✗ new |

Source: `ChessnutBLEDevice.java:39-41`.

```java
private static final UUID[] FENNOTIFYUUID     = {8261, 8262};   // notify
private static final UUID[] COMMANDNOTIFYUUID = {8271, 8273};   // notify
private static final UUID[] COMMANDWRITEUUID  = {8271, 8272};   // write
```

## Piece encoding (4-bit nibble per square)

Identical to Chessnut Air community-documented map:

| nibble | piece (FEN char) |
|-------:|------------------|
|  0     | empty (`' '`)    |
|  1     | `q` (black queen)  |
|  2     | `k` (black king)   |
|  3     | `b` (black bishop) |
|  4     | `p` (black pawn)   |
|  5     | `n` (black knight) |
|  6     | `R` (white rook)   |
|  7     | `P` (white pawn)   |
|  8     | `r` (black rook)   |
|  9     | `B` (white bishop) |
| 10     | `N` (white knight) |
| 11     | `Q` (white queen)  |
| 12     | `K` (white king)   |

Source: `ChessnutService.java:78-90` (`PIECEMAP`).

## Wire frames

All frames sent from the app to the board follow `[OPCODE] [LEN] [DATA…]`.
Length is the count of subsequent payload bytes (not including the opcode/len
themselves, in most cases — verify per-opcode).

### Init / handshake — Move

Re-reading `ChessnutBLEDevice.java:339-350`, the actual order is:

```
ChessnutBLEDevice.java:339  → 0x21 0x01 0x00                       // streaming-enable (always sent)
ChessnutBLEDevice.java:342  → 0x0B 0x04 0x03 0xE8 0x00 0xC8       // ?? handshake constants
ChessnutBLEDevice.java:350  → 0x27 0x01 0x00                       // ONLY when name lacks "Chessnut"
ChessnutBLEDevice.java:493  → 0x2B 0x0D "Chessnut Move"            // "set/identify name" (response handler)
```

So **`0x27 0x01 0x00` is NOT sent to "Chessnut Move" (or any
"Chessnut*"-named) firmware**. The earlier note in this file
treating `0x27` as the Move-specific init was wrong; the Move
firmware never sees that frame from the official Android app.
Sending it appears to be at best noise and may put the firmware
into a confused state — earlier desktop builds that included
this third write got `ACK FEN_FORCE` from the board but no motor
movement.

Both `0x21` and `0x27` are used in the codebase for *something*;
`0x27` is just the legacy unbranded-firmware handshake. Skip it.

### LED control

```
ChessnutService.java:230,257  → 0x0A 0x08 [8 bytes of bitmask]      // 1 bit per square; row-major from h1
```

Same `0x0A` length-8 LED format as Air.

### Board-state read (32-byte 4bpp, with header)

Confirmed format from `ChessnutService.java:1011-1015` (`onFenDataListener`)
plus `formatFen` at `:742`:

```
[0]      0x01                  ← opcode "FEN data"
[1]      0x24  (= 36)          ← payload length in bytes (32 board + 4 trailer)
[2..33]  32 bytes              ← 64 squares packed two-per-byte (high nibble
                                 first), pair-reversed within each rank — same
                                 packing as the outbound 0x42 setMoveBoard
                                 frame's bytes [2..33]
[34..37] 4 bytes               ← trailer (status / battery-level — value
                                 unused here)
```

Total frame is 38 bytes. Decoders MUST skip the 2-byte header before reading
the board nibbles — reading from offset 0 misaligns the entire grid by 2
bytes and surfaces as ~31 bogus square diffs on every move.

The Java decoder dispatch test is `bytes[0] == 0x01 && bytes[1] == 0x24` —
non-board notify frames (e.g. `0x41 0x0B` getMovePieceState replies, `0x28`
acks, `0x2A` power-level pushes) share the same characteristic and must be
filtered out by their own opcode.

### Move-specific: `setMoveBoard(fen, force)` — the motor command

This is the headline finding. The app sends a *target* board state and
the firmware plans the motor moves itself.

Source: `ChessnutService.java:330-350`.

Frame format (35 bytes):
```
[0]  0x42                  ← opcode "set move board"
[1]  0x21  (= 33)          ← payload length
[2..33]  32 bytes           ← target board, 4 bits per square, same packing as 0x43 LED-color
[34]    force flag         ← 0 = force (always replan), 1 = soft (only if state changed)
```

The 32-byte board encoding loops `i = 0..7` (rows) and `i2 = 0..3` (column
pairs). Index calculation: `bArr[(i * 4) + (3 - i2) + 2]`, so each row's
4 bytes are written **in reverse column order** (h-pair first, a-pair last).
That matters — straight-forward a→h ordering won't work.

To play a move from your code you don't need to send "drag e2→e4". Just
compute the new FEN after the move and call `setMoveBoard(new_fen, force=0)`.

### Move-specific: status / channel / Wi-Fi / firmware

All sub-commanded under opcode `0x41` with a one-byte selector:

| Bytes               | Meaning                                |
|---------------------|----------------------------------------|
| `0x41 0x01 0x01`    | get Wi-Fi IP                           |
| `0x41 0x01 0x05`    | get firmware version                   |
| `0x41 0x01 0x09`    | start Wi-Fi firmware update            |
| `0x41 0x01 0x0A`    | get Wi-Fi SSID                         |
| `0x41 0x01 0x0B`    | get piece state (which pieces present) |
| `0x41 0x01 0x0C`    | (queried by `setMoveBoardChannel`)     |
| `0x41 0x01 0x10`    | (Wi-Fi switch state)                   |
| `0x41 0x01 0x11`    | (board-channel get)                    |
| `0x41 0x01 0x13`    | (board-channel settings get)           |
| `0x41 0x01 0x19`    | turn off all car motors                |
| `0x41 0x01 0x1B`    | get car channel on board               |
| `0x41 0x01 0x1C`    | (board-channel settings save?)         |
| `0x41 0x01 0x1F`    | get board channel                      |
| `0x41 0x02 0x14 cc` | set board channel = `cc`               |
| `0x41 0x02 0x18 0`  | (Wi-Fi connect)                        |
| `0x41 0x02 0x1E b`  | set Wi-Fi switch (`b` = on/off)        |
| `0x41 0x02 0x20 cc` | shutdown specific board channel `cc`   |
| `0x41 0x02 0x0F cc` | set board-channel-settings entry       |
| `0x41 (n+1) 0x03 …` | set Wi-Fi SSID (`n`-byte string)       |

(Sub-codes catalogued by grepping `writeData(new byte[]{65, …})` in
`ChessnutService.java`.)

### Other named methods worth noting

```
setMoveBoard(fen, force)        // motor command — see above
getMovePieceState()             // 0x41 0x01 0x0B
turnOffMoveAllCar()             // 0x41 0x01 0x19
setMoveBoardChannel(channel)    // 0x41 0x02 0x14 channel
shutdownMoveBoardChannel(ch)    // 0x41 0x02 0x20 channel
startUpdateMoveFWViaBle(...)    // BLE OTA firmware
updateMoveFWViaBle(data)        // BLE OTA payload chunks
```

## How this maps onto integration with the desktop chess app

For our use case (drive the Move from the existing `chess` binary on a
move-by-move basis):

1. **Connect** to the device advertising `Chessnut Move`. Bond if needed.
2. **Subscribe** to notifications on `8262` (board state) and `8273`
   (command responses). Optionally also `8261` and `8271` for the
   Move-only channels — ack/status frames may arrive there.
3. **Init** by writing `0x21 0x01 0x00` (streaming-enable) and then
   `0x0B 0x04 0x03 0xE8 0x00 0xC8`. Do *not* send `0x27 0x01 0x00`
   to a "Chessnut Move"-named device — Android skips it and so
   should we (see Init / handshake section above).
4. **Drive moves** by recomputing the FEN after `execute_move()` in
   `app_state.cpp` and writing `0x42 0x21 [32 bytes target board] [force]`
   on `8272`.
5. **Light up squares** (legal-move hints, last-move highlight) by writing
   `0x0A 0x08 [bitmask]` on `8272`.

We do **not** need to compute motor paths — the firmware does that. Our
only job is the board encoding (4 bits per square + reversed-column-pair
ordering) and dispatching writes.

## Open questions to verify with an HCI snoop log

- The `8261` / `8271` channels are listed but I haven't traced what they
  carry. Capture a few motor moves and compare to `8273` notifies.
- Confirm the 32-byte board frame from the device exactly matches the
  packing expected by `PIECEMAP` (and whether a header/trailer wraps it on
  the wire).
- Confirm the column-pair-reversed ordering by sending a known position
  and reading it back.
- The `force` flag's exact semantics — guess: 0 means "replan from
  current sensor state", 1 means "only move if differs from last cmd".

## Suggested next steps

- Prototype a Python BLE client with `bleak` that just connects and prints
  every notification on all four notify UUIDs while you drive the official
  app — fastest feedback loop for the open questions.
- Once confident, port the motor-command encoder to C++ (`bluez` /
  `simpleble`) and wire it into `app_state.cpp`'s post-move hook so every
  desktop move plays out on the physical board.
