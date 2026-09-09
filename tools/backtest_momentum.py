#!/usr/bin/env python3
"""Drive NexusLOB's fixed binary TCP protocol and validate a momentum replay."""

from __future__ import annotations

import argparse
import socket
import struct
from dataclasses import dataclass

import numpy as np

MAGIC = 0x4E4C4F42  # NLOB
NEW_ORDER = 1
BUY = 0
SELL = 1
LIMIT_GTC = 1
MARKET = 3

ORDER = struct.Struct("!IBBBBQIIQ")
EXECUTION = struct.Struct("!IBBHQQQII")


@dataclass(frozen=True)
class ExpectedFill:
    side: int
    quantity: int


def order_frame(
    order_id: int,
    side: int,
    order_type: int,
    price_tick: int,
    quantity: int,
    sequence: int,
) -> bytes:
    return ORDER.pack(MAGIC, NEW_ORDER, side, order_type, 0, order_id, price_tick, quantity, sequence)


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


def momentum_sides(sample_count: int, rng: np.random.Generator) -> np.ndarray:
    """Synthetic mid-price path with a short/long rolling-return momentum signal."""
    innovations = rng.normal(loc=0.015, scale=0.85, size=sample_count + 32)
    mid = 10_000.0 + np.cumsum(innovations)
    returns = np.diff(mid)
    fast = np.convolve(returns, np.ones(5) / 5.0, mode="valid")
    slow = np.convolve(returns, np.ones(20) / 20.0, mode="valid")
    score = fast[-sample_count:] - slow[-sample_count:]
    return np.where(score >= 0.0, BUY, SELL).astype(np.uint8)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--signals", type=int, default=5_000)
    parser.add_argument("--seed", type=int, default=7)
    args = parser.parse_args()

    rng = np.random.default_rng(args.seed)
    levels_per_side = 2_048
    resting_quantity = 200
    base_ask = 10_000
    base_bid = 9_999
    sequence = 1
    order_id = 1
    frames: list[bytes] = []

    # Seed deep two-sided liquidity. GTC additions should not produce reports.
    for offset in range(levels_per_side):
        frames.append(order_frame(order_id, SELL, LIMIT_GTC, base_ask + offset, resting_quantity, sequence))
        order_id += 1
        sequence += 1
        frames.append(order_frame(order_id, BUY, LIMIT_GTC, base_bid - offset, resting_quantity, sequence))
        order_id += 1
        sequence += 1

    expected: dict[int, ExpectedFill] = {}
    sides = momentum_sides(args.signals, rng)
    quantities = rng.integers(1, 101, size=args.signals, endpoint=False)
    for side, quantity in zip(sides.tolist(), quantities.tolist(), strict=True):
        frames.append(order_frame(order_id, side, MARKET, 0, quantity, sequence))
        expected[order_id] = ExpectedFill(side=side, quantity=quantity)
        order_id += 1
        sequence += 1

    with socket.create_connection((args.host, args.port), timeout=10.0) as connection:
        connection.settimeout(20.0)
        connection.sendall(b"".join(frames))

        cash_ticks = 0
        inventory = 0
        observed_quantity = 0
        remaining_by_aggressor = {order_id: fill.quantity for order_id, fill in expected.items()}
        last_sequence = 0
        expected_quantity = sum(fill.quantity for fill in expected.values())
        while observed_quantity < expected_quantity:
            fields = EXECUTION.unpack(receive_exact(connection, EXECUTION.size))
            magic, message_type, side, _reserved, execution_sequence, aggressor_id, _resting_id, price, quantity = fields
            assert magic == MAGIC and message_type == 0x80, "invalid execution frame"
            assert execution_sequence == last_sequence + 1, "execution sequence gap"
            assert aggressor_id in expected, "unexpected fill"
            target = expected[aggressor_id]
            assert side == target.side and 0 < quantity <= remaining_by_aggressor[aggressor_id], "incorrect fill"

            remaining_by_aggressor[aggressor_id] -= quantity
            last_sequence = execution_sequence
            observed_quantity += quantity
            if side == BUY:
                inventory += quantity
                cash_ticks -= price * quantity
            else:
                inventory -= quantity
                cash_ticks += price * quantity

    assert all(quantity == 0 for quantity in remaining_by_aggressor.values())
    assert observed_quantity == expected_quantity

    # Mark inventory to the synthetic midpoint; one tick is one cent here.
    marked_pnl_dollars = (cash_ticks + inventory * 10_000) / 100.0
    print(
        f"validated {args.signals:,} momentum orders / {observed_quantity:,} shares; "
        f"inventory={inventory:,}; marked P&L=${marked_pnl_dollars:,.2f}"
    )


if __name__ == "__main__":
    main()
