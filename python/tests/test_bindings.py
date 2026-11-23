"""Tests for the ctypes bindings: semantics survive the FFI boundary."""

import numpy as np
import pytest

from tickforge import BUY, SELL, FlowGenerator, OrderBook, Status, Stp, Tif


@pytest.fixture
def book():
    with OrderBook(0, 1000, 1024) as b:
        yield b


def test_match_and_trade_buffer(book):
    assert book.limit(1, SELL, 100, 5).ok
    assert book.limit(2, SELL, 101, 5).ok
    r = book.limit(3, BUY, 101, 7)
    assert (r.status, r.filled, r.resting) == (Status.OK, 7, 0)
    t = book.take_trades()
    assert t["maker_id"].tolist() == [1, 2]
    assert t["price"].tolist() == [100, 101]
    assert t["qty"].tolist() == [5, 2]
    assert (t["taker_side"] == BUY).all()
    assert book.take_trades().size == 0  # buffer cleared


def test_market_data(book):
    assert book.best_bid is None and book.mid is None
    book.limit(1, BUY, 99, 3)
    book.limit(2, BUY, 98, 4)
    book.limit(3, SELL, 102, 1)
    assert book.best_bid == 99 and book.best_ask == 102 and book.mid == 100.5
    assert book.depth(BUY, 5) == [(99, 3), (98, 4)]
    assert len(book) == 3


def test_rejections_and_tif(book):
    assert book.limit(1, BUY, 2000, 1).status == Status.REJECTED_INVALID_PRICE
    assert book.cancel(42).status == Status.REJECTED_UNKNOWN_ORDER
    book.limit(1, SELL, 100, 2)
    assert book.limit(2, BUY, 100, 5, Tif.FOK).status == Status.REJECTED_FOK_UNFILLABLE
    r = book.limit(3, BUY, 100, 5, Tif.IOC)
    assert r.filled == 2 and r.resting == 0 and len(book) == 0


def test_generator_is_reproducible():
    def run(seed):
        with OrderBook() as b:
            g = FlowGenerator(seed)
            g.step(b, 20_000)
            return b.best_bid, b.best_ask, len(b), b.take_trades()["qty"].sum(), g.fair_value

    assert run(5) == run(5)
    assert run(5) != run(6)


def test_generator_ids_do_not_collide_with_user_ids():
    with OrderBook() as b:
        g = FlowGenerator(1, first_id=1)
        g.step(b, 5_000)
        my_id = 2**62
        assert b.limit(my_id, BUY, b.best_bid, 1).ok
        g.step(b, 5_000)
        trades = b.take_trades()
        assert np.all((trades["maker_id"] < 2**62) | (trades["maker_id"] == my_id))


def test_invalid_side_and_tif_rejected_before_c(book):
    with pytest.raises(ValueError):
        book.limit(1, 2, 100, 1)
    with pytest.raises(ValueError):
        book.market(1, -1, 1)
    with pytest.raises(ValueError):
        book.limit(1, BUY, 100, 1, 7)  # not a Tif
    with pytest.raises(ValueError):
        book.depth(5)
    assert len(book) == 0


def test_market_order_cannot_reuse_live_id(book):
    book.limit(1, SELL, 100, 5)
    book.limit(2, BUY, 90, 5)
    assert book.market(2, BUY, 1).status == Status.REJECTED_DUPLICATE_ID
    assert book.market(3, BUY, 1).filled == 1


def test_quantity_cap(book):
    assert book.limit(1, BUY, 100, 2**30 + 1).status == Status.REJECTED_INVALID_QTY
    assert book.limit(1, BUY, 100, 2**30).ok


def test_closed_book_raises_instead_of_crashing():
    b = OrderBook(0, 1000, 16)
    b.close()
    b.close()  # idempotent
    for call in (lambda: b.limit(1, BUY, 1, 1), lambda: b.cancel(1), lambda: b.best_bid,
                 lambda: len(b), lambda: b.take_trades(), lambda: b.depth(BUY)):
        with pytest.raises(ValueError):
            call()
    g = FlowGenerator(1)
    with pytest.raises(ValueError):
        g.step(b, 10)
    g.close()
    with pytest.raises(ValueError):
        _ = g.fair_value


def test_self_trade_prevention(book):
    book.limit(1, SELL, 100, 3, owner=9)
    book.limit(2, SELL, 100, 4, owner=7)
    book.limit(3, SELL, 101, 5, owner=9)
    r = book.limit(10, BUY, 101, 10, owner=7, stp=Stp.CANCEL_OLDEST)
    assert (r.filled, r.resting, r.self_trade_cancelled) == (8, 2, 0)
    assert book.take_trades()["maker_id"].tolist() == [1, 3]
    cx = book.take_self_trade_cancels()
    assert cx["order_id"].tolist() == [2] and cx["qty"].tolist() == [4]
    assert book.take_self_trade_cancels().size == 0  # buffer cleared

    book.limit(20, SELL, 105, 5, owner=7)
    r = book.market(21, BUY, 9, owner=7)  # CANCEL_NEWEST by default
    assert (r.filled, r.self_trade_cancelled) == (0, 9)
    assert len(book) == 2


def test_owner_must_fit_32_bits(book):
    with pytest.raises(ValueError):
        book.limit(1, BUY, 100, 1, owner=2**32)


def test_generator_only_touches_live_orders():
    with OrderBook() as b:
        g = FlowGenerator(3)
        g.step(b, 50_000)
        # Every order on the book is the generator's, and it tracks them all,
        # so depth is stationary: about 55% of the 4000 target with defaults.
        assert 1600 < len(b) < 3000
