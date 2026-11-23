"""tickforge — Python interface to the C++ matching engine.

The engine is loaded from ``libtickforge_c.so`` (built by CMake into this
package directory) through ctypes, so no compiled Python extension or third
party binding library is needed.
"""

from tickforge.book import (
    BUY,
    SELL,
    FlowGenerator,
    OrderBook,
    Report,
    Status,
    Stp,
    Tif,
    TradeArray,
)

__all__ = ["BUY", "SELL", "FlowGenerator", "OrderBook", "Report", "Status", "Stp", "Tif", "TradeArray"]
__version__ = "1.1.0"
