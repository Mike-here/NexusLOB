#!/usr/bin/env python3
"""Replay a dynamically quoted momentum strategy through NexusLOB's binary TCP API."""

from __future__ import annotations

import argparse
import socket
import struct
from dataclasses import dataclass

import numpy as np

MAGIC = 0x4E4C4F42  # NLOB
NEW_ORDER = 1
CANCEL_ORDER = 2
BUY = 0
SELL = 1
LIMIT_GTC = 1
MARKET = 3

ORDER = struct.Struct("!IBBBBQIIQ")
EXECUTION = struct.Struct("!IBBHQQQII")
QUOTE_QUANTITY = 128
ORDER_ID_STRIDE = 16_000
ORDER_ID_NAMESPACES = 250


@dataclass(frozen=True)
class ExpectedFill:
    side: int
    quantity: int


def order_frame(
    message_type: int,
    order_id: int,
    side: int,
    order_type: int,
    price_tick: int,
    quantity: int,
    sequence: int,
) -> bytes:
    return ORDER.pack(MAGIC, message_type, side, order_type, 0, order_id, price_tick, quantity, sequence)


def receive_exact(connection: socket.socket, byte_count: int) -> bytes:
    chunks: list[bytes] = []
    remaining = byte_count
    while remaining:
        chunk = connection.recv(remaining)
        if not chunk:
            raise ConnectionError("engine closed the socket before the expected execution stream arrived")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def make_momentum_path(sample_count: int, rng: np.random.Generator) -> tuple[np.ndarray, np.ndarray]:
    """Return executable midpoints and lagged fast-minus-slow momentum signals.

    Each signal uses returns observed before its quote midpoint, preventing a
    same-tick look-ahead bias in the toy strategy.
    """
    innovations = rng.normal(loc=0.03, scale=0.80, size=sample_count + 21)
    midpoints = 10_000.0 + np.cumsum(innovations)
    returns = np.diff(midpoints)
    fast = np.convolve(returns, np.ones(5) / 5.0, mode="valid")
    slow = np.convolve(returns, np.ones(20) / 20.0, mode="valid")
    score = fast[15 : 15 + sample_count] - slow[:sample_count]
    sides = np.where(score >= 0.0, BUY, SELL).astype(np.uint8)
    quote_midpoints = np.rint(midpoints[20 : 20 + sample_count]).astype(np.int64)
    return quote_midpoints, sides


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--signals", type=int, default=5_000)
    parser.add_argument("--seed", type=int, default=7)
    args = parser.parse_args()
    if not 1 <= args.signals <= (ORDER_ID_STRIDE - 1) // 3:
        parser.error(f"--signals must be in [1, ${(ORDER_ID_STRIDE - 1) // 3}]")

    rng = np.random.default_rng(args.seed)
    midpoint_ticks, sides = make_momentum_path(args.signals, rng)
    quantities = rng.integers(1, 101, size=args.signals)

    # A seed-specific order-ID namespace avoids collisions with a prior replay
    # once its cancel messages have drained through the matching engine.
    order_id = 1 + (args.seed % ORDER_ID_NAMESPACES) * ORDER_ID_STRIDE
    sequence = 1
    frames: list[bytes] = []
    expected: dict[int, ExpectedFill] = {}

    for midpoint, side, quantity in zip(midpoint_ticks.tolist(), sides.tolist(), quantities.tolist(), strict=True):
        bid_tick = max(1, int(midpoint) - 1)
        ask_tick = bid_tick + 2
        bid_id = order_id
        ask_id = order_id + 1
        market_id = order_id + 2
        order_id += 3

        # Quotes are replenished each event, then both residual quotes are
        # cancelled. This supplies controlled depth around the evolving mid.
        frames.append(order_frame(NEW_ORDER, bid_id, BUY, LIMIT_GTC, bid_tick, QUOTE_QUANTITY, sequence))
        sequence += 1
        frames.append(order_frame(NEW_ORDER, ask_id, SELL, LIMIT_GTC, ask_tick, QUOTE_QUANTITY, sequence))
        sequence += 1
        frames.append(order_frame(NEW_ORDER, market_id, side, MARKET, 0, quantity, sequence))
        sequence += 1
        expected[market_id] = ExpectedFill(side=side, quantity=quantity)
        frames.append(order_frame(CANCEL_ORDER, bid_id, BUY, LIMIT_GTC, 0, 0, sequence))
        sequence += 1
        frames.append(order_frame(CANCEL_ORDER, ask_id, SELL, LIMIT_GTC, 0, 0, sequence))
        sequence += 1

    with socket.create_connection((args.host, args.port), timeout=10.0) as connection:
        connection.settimeout(20.0)
        connection.sendall(b"".join(frames))

        cash_ticks = 0
        inventory = 0
        observed_quantity = 0
        expected_quantity = sum(fill.quantity for fill in expected.values())
        remaining_by_aggressor = {order_id: fill.quantity for order_id, fill in expected.items()}
        first_execution_sequence: int | None = None
        last_execution_sequence: int | None = None

        while observed_quantity < expected_quantity:
            fields = EXECUTION.unpack(receive_exact(connection, EXECUTION.size))
            magic, message_type, side, _reserved, execution_sequence, aggressor_id, _resting_id, price, quantity = fields
            assert magic == MAGIC and message_type == 0x80, "invalid execution frame"
            if first_execution_sequence is None:
                first_execution_sequence = execution_sequence
            else:
                assert last_execution_sequence is not None
                assert execution_sequence == last_execution_sequence + 1, "execution sequence gap"
            assert aggressor_id in expected, "unexpected fill"
            target = expected[aggressor_id]
            assert side == target.side and 0 < quantity <= remaining_by_aggressor[aggressor_id], "incorrect fill"

            remaining_by_aggressor[aggressor_id] -= quantity
            last_execution_sequence = execution_sequence
            observed_quantity += quantity
            if side == BUY:
                inventory += quantity
                cash_ticks -= price * quantity
            else:
                inventory -= quantity
                cash_ticks += price * quantity

    assert all(quantity == 0 for quantity in remaining_by_aggressor.values())
    mark_tick = int(midpoint_ticks[-1])
    marked_pnl_dollars = (cash_ticks + inventory * mark_tick) / 100.0
    print(
        f"validated {args.signals:,} momentum orders / {observed_quantity:,} shares; "
        f"execution sequence {first_execution_sequence}-{last_execution_sequence}; "
        f"final midpoint=${mark_tick / 100.0:,.2f}; inventory={inventory:,}; "
        f"marked P&L=${marked_pnl_dollars:,.2f}"
    )


if __name__ == "__main__":
    main()
