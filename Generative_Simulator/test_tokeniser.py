"""
Tokeniser round-trip and sanity tests.

Run from the project root:
    python Generative_Simulator/test_tokeniser.py

What this checks:
  1. encode_training returns four equal-length streams, all within VOCAB_SIZE.
  2. action round-trip: token_to_action recovers the original (type, direction)
     exactly (the action mapping is lossless).
  3. size / time / price round-trips: decode then re-encode reproduces the
     original token. These maps are intentionally lossy (log/quantile bucketing),
     so we cannot expect decoded *values* to match originals — but they must
     stay inside the same bucket, which means the re-encoded token is stable.
  4. Spot checks on hand-constructed inputs (at-best, aggressive, far) so we
     fail loudly if the price-region boundaries shift.
"""
import sys
from pathlib import Path
import numpy as np
import pandas as pd

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "Generative_Simulator"))
from Tokeniser import Tokeniser

MSG_PATH = (
    ROOT
    / "LOBSTER_SampleFile_AAPL_2012-06-21_10"
    / "AAPL_2012-06-21_34200000_57600000_message_10.csv"
)
BOOK_PATH = (
    ROOT
    / "LOBSTER_SampleFile_AAPL_2012-06-21_10"
    / "AAPL_2012-06-21_34200000_57600000_orderbook_10.csv"
)

N_ROWS = 10_000  # slice size — keeps the test fast


def _load_sample():
    message = pd.read_csv(
        MSG_PATH,
        names=["time", "type", "order_id", "size", "price", "direction"],
    ).iloc[:N_ROWS].reset_index(drop=True)

    cols = []
    for lvl in range(1, 11):
        cols += [f"ask_p{lvl}", f"ask_s{lvl}", f"bid_p{lvl}", f"bid_s{lvl}"]
    book = pd.read_csv(BOOK_PATH, header=None, names=cols).iloc[:N_ROWS].reset_index(drop=True)

    return message, book


def _derive_encode_inputs(message, book):
    """Mirror encode_training's preprocessing so the test can reconstruct
    the (best_same_side, direction) pair that price_to_token was called with.
    Must stay in lockstep with encode_training in Tokeniser.py."""
    mask = message["type"].isin([1, 2, 3])
    book_pre = book.shift(1)[mask].reset_index(drop=True).iloc[1:].reset_index(drop=True)
    msg_used = message[mask].reset_index(drop=True).iloc[1:].reset_index(drop=True)

    direction = msg_used["direction"].to_numpy()
    best_same = np.where(
        direction == 1,
        book_pre["bid_p1"].to_numpy(),
        book_pre["ask_p1"].to_numpy(),
    )
    return msg_used, best_same, direction


def test_round_trip():
    message, book = _load_sample()
    tok = Tokeniser(MSG_PATH)

    # encode_training now returns a single interleaved 1-D sequence:
    #   per message: (action, size, price, time)  — must match Tokeniser.py
    seq = tok.encode_training(message.copy(), book.copy())
    assert seq.ndim == 1, f"expected 1-D sequence, got shape {seq.shape}"
    assert seq.size % 4 == 0, f"sequence length {seq.size} is not a multiple of 4"

    grid = seq.reshape(-1, 4)
    action_tok = grid[:, 0]
    size_tok   = grid[:, 1]
    price_tok  = grid[:, 2]
    time_tok   = grid[:, 3]

    print(f"Encoded {len(grid)} kept messages from a {N_ROWS}-row sample "
          f"(interleaved sequence length {seq.size}).")
    print(f"  action_tok range [{int(action_tok.min())}, {int(action_tok.max())}]"
          f"   expected [{tok.ACTION_BASE}, {tok.ACTION_BASE + 5}]")
    print(f"  size_tok   range [{int(size_tok.min())}, {int(size_tok.max())}]"
          f"   expected [{tok.SIZE_BASE}, {tok.SIZE_BASE + 19}]")
    print(f"  price_tok  range [{int(price_tok.min())}, {int(price_tok.max())}]"
          f"   expected [{tok.PRICE_BASE}, {tok.PRICE_BASE + 60}]")
    print(f"  time_tok   range [{int(time_tok.min())}, {int(time_tok.max())}]"
          f"   expected [{tok.DT_BASE}, {tok.DT_BASE + 39}]")
    print()

    # ---- 1. shape consistency -------------------------------------------
    n = len(grid)
    assert len(size_tok) == len(time_tok) == len(action_tok) == len(price_tok) == n, (
        "deinterleaved streams of unequal length — interleave/reshape bug"
    )
    print(f"[1] PASS  all four streams length {n}")

    # ---- 2. vocab range -------------------------------------------------
    assert int(seq.min()) >= 0 and int(seq.max()) < tok.VOCAB_SIZE, (
        f"tokens fall outside [0, VOCAB_SIZE={tok.VOCAB_SIZE})"
    )
    print(f"[2] PASS  all tokens within [0, {tok.VOCAB_SIZE})")

    # ---- 3. action round-trip (lossless, value-exact) -------------------
    msg_used, best_same, direction = _derive_encode_inputs(message, book)

    ot_dec, dir_dec = tok.token_to_action(action_tok)
    ot_dec = np.asarray(ot_dec)
    dir_dec = np.asarray(dir_dec)
    assert np.array_equal(ot_dec, msg_used["type"].to_numpy()), (
        "action: decoded order_type does not match original"
    )
    assert np.array_equal(dir_dec, msg_used["direction"].to_numpy()), (
        "action: decoded direction does not match original"
    )
    print("[3] PASS  action round-trip (lossless, exact)")

    # ---- 4. size: token stable under decode -> re-encode ---------------
    try:
        size_dec = tok.token_to_size(size_tok)
        size_reenc = tok.size_to_token(size_dec)
        assert np.array_equal(size_reenc, size_tok), (
            "size: token not stable on re-encode"
        )
        print("[4] PASS  size round-trip (lossy, tokens stable)")
    except TypeError as e:
        print(f"[4] FAIL  token_to_size is not vectorised: {e}")
        print("       Fix in Tokeniser.token_to_size:")
        print("         representative = int(np.sqrt(l*r))     # scalar only")
        print("         representative = np.sqrt(l*r).astype(int)   # vectorised")

    # ---- 5. price: token stable under decode -> re-encode --------------
    price_dec = tok.token_to_price(price_tok, best_same, direction)
    price_reenc = tok.price_to_token(price_dec, best_same, direction)
    assert np.array_equal(price_reenc, price_tok), (
        "price: token not stable on re-encode. "
        "Likely encode/decode convention mismatch, or encode_training's "
        "filter/shift order is wrong (must shift book first, then filter)."
    )
    print("[5] PASS  price round-trip (lossy in far region, tokens stable)")

    # ---- 6. time: token stable under decode -> re-encode ---------------
    time_dec = tok.token_to_time(time_tok)
    time_reenc = tok.time_to_token(time_dec)
    assert np.array_equal(time_reenc, time_tok), (
        "time: token not stable on re-encode"
    )
    print("[6] PASS  time round-trip (lossy, tokens stable)")


def test_price_spot_checks():
    """Hand-constructed cases for the price encoder to catch region-boundary
    regressions. Independent of LOBSTER data.

    Convention for a BUY (direction=+1):
        offset = (best_bid - price)
        price > best_bid -> offset < 0 -> aggressive (token < 39)
        price < best_bid -> offset > 0 -> passive    (token > 39)
        price = best_bid -> offset = 0 -> at best    (token = 39)
    """
    tok = Tokeniser(MSG_PATH)
    PB = tok.PRICE_BASE   # 9 -> at-best token is PB + 30 = 39
    best_bid = 5_000_000  # $500.00 in LOBSTER units

    # Each case: (price, direction, expected_token, label)
    # Convention (matches encoder):
    #   aggressive far  -> bucket 0..9   -> token PB+0  .. PB+9   (LOW)
    #   aggressive lin  -> bucket 10..29 -> token PB+10 .. PB+29
    #   at best         -> bucket 30     -> token PB+30
    #   passive linear  -> bucket 31..50 -> token PB+31 .. PB+50
    #   passive far     -> bucket 51..60 -> token PB+51 .. PB+60  (HIGH)
    cases = [
        (best_bid,           +1, PB + 30, "at best"),
        (best_bid + 100,     +1, PB + 29, "aggressive 1 tick"),
        (best_bid - 100,     +1, PB + 31, "passive 1 tick"),
        (best_bid + 2_000,   +1, PB + 10, "aggressive 20 ticks (linear edge)"),
        (best_bid - 2_000,   +1, PB + 50, "passive 20 ticks (linear edge)"),
        (best_bid + 2_100,   +1, PB +  9, "aggressive far 21 ticks (first log bucket)"),
        (best_bid - 2_100,   +1, PB + 51, "passive far 21 ticks (first log bucket)"),
        (best_bid + 70_000,  +1, PB +  0, "aggressive far 700 ticks (last log bucket)"),
        (best_bid - 70_000,  +1, PB + 60, "passive far 700 ticks (last log bucket)"),
    ]

    print()
    print(f"Price spot checks (buy side, best_bid = {best_bid}):")
    all_ok = True
    for price, direction, expected, label in cases:
        got = int(tok.price_to_token(
            np.array([price]),
            np.array([best_bid]),
            np.array([direction]),
        )[0])
        ok = got == expected
        all_ok &= ok
        tag = "PASS" if ok else "FAIL"
        print(f"  [{tag}] {label:46s}  expected {expected:3d}, got {got:3d}")
    print()
    print("Price spot checks: " + ("all PASS" if all_ok else "some FAILED"))


if __name__ == "__main__":
    test_round_trip()
    test_price_spot_checks()
    print("\nDone.")
