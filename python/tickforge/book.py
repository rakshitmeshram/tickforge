"""ctypes bindings for the tickforge C ABI (include/tickforge/c_api.h)."""

from __future__ import annotations

import ctypes as C
import enum
import os
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np

BUY = 0
SELL = 1
NO_PRICE = -(2**63)


class Tif(enum.IntEnum):
    GTC = 0
    IOC = 1
    FOK = 2


class Stp(enum.IntEnum):
    """Self-trade prevention mode, applied when an order's owner is non-zero."""

    CANCEL_NEWEST = 0  # cancel the incoming order's remainder
    CANCEL_OLDEST = 1  # cancel the resting order, keep matching
    CANCEL_BOTH = 2    # cancel both


class Status(enum.IntEnum):
    OK = 0
    REJECTED_DUPLICATE_ID = 1
    REJECTED_INVALID_PRICE = 2
    REJECTED_INVALID_QTY = 3
    REJECTED_UNKNOWN_ORDER = 4
    REJECTED_FOK_UNFILLABLE = 5
    REJECTED_CAPACITY = 6
    REJECTED_INVALID_MESSAGE = 7


class _Trade(C.Structure):
    _fields_ = [
        ("maker_id", C.c_uint64),
        ("taker_id", C.c_uint64),
        ("price", C.c_int64),
        ("qty", C.c_int64),
        ("taker_side", C.c_uint8),
    ]


class _Report(C.Structure):
    _fields_ = [("status", C.c_int32), ("filled", C.c_int64), ("resting", C.c_int64),
                ("self_trade_cancelled", C.c_int64)]


class _StpCancel(C.Structure):
    _fields_ = [("order_id", C.c_uint64), ("qty", C.c_int64)]


TRADE_DTYPE = np.dtype(
    [("maker_id", "<u8"), ("taker_id", "<u8"), ("price", "<i8"), ("qty", "<i8"), ("taker_side", "u1")],
    align=True,
)
assert TRADE_DTYPE.itemsize == C.sizeof(_Trade), "tf_trade layout mismatch"

STP_CANCEL_DTYPE = np.dtype([("order_id", "<u8"), ("qty", "<i8")], align=True)
assert STP_CANCEL_DTYPE.itemsize == C.sizeof(_StpCancel), "tf_stp_cancel layout mismatch"

TradeArray = np.ndarray  # structured array with TRADE_DTYPE


@dataclass(frozen=True, slots=True)
class Report:
    status: Status
    filled: int
    resting: int
    self_trade_cancelled: int = 0  # incoming qty cancelled by self-trade prevention

    @property
    def ok(self) -> bool:
        return self.status == Status.OK


def _find_library() -> Path:
    override = os.environ.get("TICKFORGE_LIB")
    candidates = [Path(override)] if override else []
    here = Path(__file__).resolve().parent
    if sys.platform == "win32":
        # MSVC names the DLL tickforge_c.dll; MinGW adds a "lib" prefix.
        names = ["tickforge_c.dll", "libtickforge_c.dll"]
    elif sys.platform == "darwin":
        names = ["libtickforge_c.dylib", "libtickforge_c.so"]
    else:
        names = ["libtickforge_c.so"]
    candidates += [here / n for n in names]
    for c in candidates:
        if c.exists():
            return c
    raise OSError(
        f"tickforge_c shared library not found in {here}. Build it first:\n"
        "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j --config Release"
    )


def _load() -> C.CDLL:
    path = _find_library()
    if sys.platform == "win32" and hasattr(os, "add_dll_directory"):
        # Let Windows resolve any runtime DLLs that sit next to the library.
        os.add_dll_directory(str(path.parent))
    lib = C.CDLL(str(path))
    p_book, p_gen = C.c_void_p, C.c_void_p
    sig = {
        "tf_book_new": (p_book, [C.c_int64, C.c_int64, C.c_uint64]),
        "tf_book_free": (None, [p_book]),
        "tf_book_limit": (_Report, [p_book, C.c_uint64, C.c_uint8, C.c_int64, C.c_int64, C.c_uint8]),
        "tf_book_market": (_Report, [p_book, C.c_uint64, C.c_uint8, C.c_int64]),
        "tf_book_limit_ex": (_Report, [p_book, C.c_uint64, C.c_uint8, C.c_int64, C.c_int64, C.c_uint8,
                                       C.c_uint32, C.c_uint8]),
        "tf_book_market_ex": (_Report, [p_book, C.c_uint64, C.c_uint8, C.c_int64, C.c_uint32, C.c_uint8]),
        "tf_book_self_trade_cancels": (C.c_uint64, [p_book, C.POINTER(C.POINTER(_StpCancel))]),
        "tf_book_clear_self_trade_cancels": (None, [p_book]),
        "tf_book_cancel": (_Report, [p_book, C.c_uint64]),
        "tf_book_amend": (_Report, [p_book, C.c_uint64, C.c_int64, C.c_int64]),
        "tf_book_best_bid": (C.c_int64, [p_book]),
        "tf_book_best_ask": (C.c_int64, [p_book]),
        "tf_book_order_count": (C.c_uint64, [p_book]),
        "tf_book_depth": (C.c_uint64, [p_book, C.c_uint8, C.c_uint64, C.POINTER(C.c_int64), C.POINTER(C.c_int64)]),
        "tf_book_trades": (C.c_uint64, [p_book, C.POINTER(C.POINTER(_Trade))]),
        "tf_book_clear_trades": (None, [p_book]),
        "tf_gen_new": (p_gen, [C.c_uint64, C.c_double, C.c_double, C.c_double, C.c_uint64]),
        "tf_gen_free": (None, [p_gen]),
        "tf_gen_fair_value": (C.c_double, [p_gen]),
        "tf_gen_step": (C.c_uint64, [p_gen, p_book, C.c_uint64]),
    }
    for name, (res, args) in sig.items():
        fn = getattr(lib, name)
        fn.restype = res
        fn.argtypes = args
    return lib


_lib = _load()


def _report(r: _Report) -> Report:
    return Report(Status(r.status), r.filled, r.resting, r.self_trade_cancelled)


def _owner(owner: int) -> int:
    if not 0 <= owner < 2**32:
        raise ValueError(f"owner must fit in 32 bits, got {owner!r}")
    return owner


def _side(side: int) -> int:
    if side not in (BUY, SELL):
        raise ValueError(f"side must be BUY (0) or SELL (1), got {side!r}")
    return side


class OrderBook:
    """Price-time priority limit order book backed by the C++ engine.

    Trades generated by any call accumulate in an internal buffer; fetch them
    with :meth:`take_trades`, which returns a NumPy structured array and
    clears the buffer.

    Using a book after :meth:`close` raises ``ValueError`` instead of passing
    a dangling handle to C.
    """

    def __init__(self, min_price: int = 1, max_price: int = 19_999, max_orders: int = 1 << 20):
        self._h = _lib.tf_book_new(min_price, max_price, max_orders)
        if not self._h:
            raise ValueError("invalid book configuration")
        self._depth_px = (C.c_int64 * 64)()
        self._depth_qty = (C.c_int64 * 64)()

    @property
    def _handle(self) -> int:
        h = getattr(self, "_h", None)
        if not h:
            raise ValueError("operation on a closed OrderBook")
        return h

    def close(self) -> None:
        if getattr(self, "_h", None):
            _lib.tf_book_free(self._h)
            self._h = None

    def __del__(self) -> None:
        self.close()

    def __enter__(self) -> OrderBook:
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # orders -------------------------------------------------------------
    def limit(self, order_id: int, side: int, price: int, qty: int, tif: Tif = Tif.GTC,
              owner: int = 0, stp: Stp = Stp.CANCEL_NEWEST) -> Report:
        """Limit order. A non-zero ``owner`` enables self-trade prevention:
        it never trades against a resting order of the same owner; ``stp``
        picks what is cancelled instead (see :class:`Stp`)."""
        if owner:
            return _report(_lib.tf_book_limit_ex(self._handle, order_id, _side(side), price, qty, int(Tif(tif)),
                                                 _owner(owner), int(Stp(stp))))
        return _report(_lib.tf_book_limit(self._handle, order_id, _side(side), price, qty, int(Tif(tif))))

    def market(self, order_id: int, side: int, qty: int, owner: int = 0,
               stp: Stp = Stp.CANCEL_NEWEST) -> Report:
        if owner:
            return _report(_lib.tf_book_market_ex(self._handle, order_id, _side(side), qty, _owner(owner),
                                                  int(Stp(stp))))
        return _report(_lib.tf_book_market(self._handle, order_id, _side(side), qty))

    def cancel(self, order_id: int) -> Report:
        return _report(_lib.tf_book_cancel(self._handle, order_id))

    def amend(self, order_id: int, price: int, qty: int) -> Report:
        return _report(_lib.tf_book_amend(self._handle, order_id, price, qty))

    # market data --------------------------------------------------------
    @property
    def best_bid(self) -> int | None:
        p = _lib.tf_book_best_bid(self._handle)
        return None if p == NO_PRICE else p

    @property
    def best_ask(self) -> int | None:
        p = _lib.tf_book_best_ask(self._handle)
        return None if p == NO_PRICE else p

    @property
    def mid(self) -> float | None:
        b, a = self.best_bid, self.best_ask
        return None if b is None or a is None else 0.5 * (b + a)

    def __len__(self) -> int:
        return _lib.tf_book_order_count(self._handle)

    def depth(self, side: int, levels: int = 5) -> list[tuple[int, int]]:
        n = _lib.tf_book_depth(self._handle, _side(side), max(0, min(levels, 64)), self._depth_px,
                               self._depth_qty)
        return [(self._depth_px[i], self._depth_qty[i]) for i in range(n)]

    def take_trades(self) -> np.ndarray:
        ptr = C.POINTER(_Trade)()
        h = self._handle
        n = _lib.tf_book_trades(h, C.byref(ptr))
        if n == 0:
            out = np.empty(0, dtype=TRADE_DTYPE)
        else:
            raw = C.string_at(ptr, n * C.sizeof(_Trade))
            out = np.frombuffer(raw, dtype=TRADE_DTYPE).copy()
        _lib.tf_book_clear_trades(h)
        return out

    def take_self_trade_cancels(self) -> np.ndarray:
        """Resting orders cancelled by self-trade prevention since the last
        call, as a structured array (order_id, qty); clears the buffer."""
        h = self._handle
        ptr = C.POINTER(_StpCancel)()
        n = _lib.tf_book_self_trade_cancels(h, C.byref(ptr))
        if n == 0:
            out = np.empty(0, dtype=STP_CANCEL_DTYPE)
        else:
            raw = C.string_at(ptr, n * C.sizeof(_StpCancel))
            out = np.frombuffer(raw, dtype=STP_CANCEL_DTYPE).copy()
        _lib.tf_book_clear_self_trade_cancels(h)
        return out


class FlowGenerator:
    """Background order flow from the C++ FlowGenerator, applied to a book.

    Generated order ids start at ``first_id``; keep your own ids outside that
    range (e.g. >= 2**62) to tell your fills apart.
    """

    def __init__(self, seed: int, sigma: float = 0.08, p_market: float = 0.06,
                 informed_k: float = 1.5, first_id: int = 1):
        self._h = _lib.tf_gen_new(seed, sigma, p_market, informed_k, first_id)
        if not self._h:
            raise MemoryError("tf_gen_new failed")

    @property
    def _handle(self) -> int:
        h = getattr(self, "_h", None)
        if not h:
            raise ValueError("operation on a closed FlowGenerator")
        return h

    def step(self, book: OrderBook, n: int = 1) -> None:
        _lib.tf_gen_step(self._handle, book._handle, n)

    @property
    def fair_value(self) -> float:
        return _lib.tf_gen_fair_value(self._handle)

    def close(self) -> None:
        if getattr(self, "_h", None):
            _lib.tf_gen_free(self._h)
            self._h = None

    def __del__(self) -> None:
        self.close()
