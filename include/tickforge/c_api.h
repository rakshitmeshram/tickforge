/* tickforge C ABI — a stable, exception-free boundary used by the Python
 * package (ctypes) and usable from any FFI. All functions are noexcept on the
 * C++ side; errors are reported through status codes.
 *
 * Input validation: a side byte other than 0/1, a tif byte other than 0/1/2,
 * an unknown message type, or a NULL book handle yields status 7
 * (RejectedInvalidMessage) rather than undefined behaviour.
 */
#ifndef TICKFORGE_C_API_H
#define TICKFORGE_C_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#  if defined(TF_BUILDING_DLL)
#    define TF_API __declspec(dllexport)
#  else
#    define TF_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define TF_API __attribute__((visibility("default")))
#else
#  define TF_API
#endif

typedef struct tf_book tf_book;
typedef struct tf_gen tf_gen;

typedef struct {
    uint64_t maker_id;
    uint64_t taker_id;
    int64_t price;
    int64_t qty;
    uint8_t taker_side; /* 0 = buy, 1 = sell */
} tf_trade;

typedef struct {
    int32_t status; /* 0 = ok, see tickforge::Status (7 = invalid message) */
    int64_t filled;
    int64_t resting;
    int64_t self_trade_cancelled; /* incoming qty cancelled by self-trade prevention */
} tf_report;

/* A resting order cancelled by self-trade prevention. */
typedef struct {
    uint64_t order_id;
    int64_t qty;
} tf_stp_cancel;

/* Self-trade prevention modes (used only when owner != 0). */
#define TF_STP_CANCEL_NEWEST 0
#define TF_STP_CANCEL_OLDEST 1
#define TF_STP_CANCEL_BOTH 2

typedef struct {
    uint8_t type; /* 0 limit, 1 market, 2 cancel, 3 amend */
    uint8_t side;
    uint8_t tif;
    uint8_t stp;    /* TF_STP_*; formerly padding, so zero = cancel newest */
    uint32_t owner; /* 0 = anonymous (no self-trade prevention) */
    uint64_t id;
    int64_t price;
    int64_t qty;
} tf_msg;

#define TF_NO_PRICE INT64_MIN

TF_API tf_book* tf_book_new(int64_t min_price, int64_t max_price, uint64_t max_orders);
TF_API void tf_book_free(tf_book* b);

/* Trades produced by any call are appended to the book's trade buffer; read
 * them with tf_book_trades and reset with tf_book_clear_trades. */
TF_API tf_report tf_book_limit(tf_book* b, uint64_t id, uint8_t side, int64_t price, int64_t qty, uint8_t tif);
TF_API tf_report tf_book_market(tf_book* b, uint64_t id, uint8_t side, int64_t qty);
/* As above, for an order belonging to `owner` with self-trade prevention mode
 * `stp`. Resting orders cancelled by STP go to a separate buffer read with
 * tf_book_self_trade_cancels. */
TF_API tf_report tf_book_limit_ex(tf_book* b, uint64_t id, uint8_t side, int64_t price, int64_t qty, uint8_t tif,
                                  uint32_t owner, uint8_t stp);
TF_API tf_report tf_book_market_ex(tf_book* b, uint64_t id, uint8_t side, int64_t qty, uint32_t owner, uint8_t stp);
TF_API tf_report tf_book_cancel(tf_book* b, uint64_t id);
TF_API tf_report tf_book_amend(tf_book* b, uint64_t id, int64_t price, int64_t qty);
TF_API tf_report tf_book_apply(tf_book* b, const tf_msg* m);

TF_API int64_t tf_book_best_bid(const tf_book* b);
TF_API int64_t tf_book_best_ask(const tf_book* b);
TF_API uint64_t tf_book_order_count(const tf_book* b);
/* Fills up to n levels (best first, at most 64 per call); returns the number
 * written. Returns 0 for an invalid side or NULL book. */
TF_API uint64_t tf_book_depth(const tf_book* b, uint8_t side, uint64_t n, int64_t* prices, int64_t* qtys);

TF_API uint64_t tf_book_trades(const tf_book* b, const tf_trade** out);
TF_API void tf_book_clear_trades(tf_book* b);
TF_API uint64_t tf_book_self_trade_cancels(const tf_book* b, const tf_stp_cancel** out);
TF_API void tf_book_clear_self_trade_cancels(tf_book* b);

/* Synthetic flow generator bound to a book: tf_gen_step generates the next
 * background message from the book's current top of book and applies it. */
TF_API tf_gen* tf_gen_new(uint64_t seed, double sigma, double p_market, double informed_k, uint64_t first_id);
TF_API void tf_gen_free(tf_gen* g);
TF_API double tf_gen_fair_value(const tf_gen* g);
TF_API uint64_t tf_gen_step(tf_gen* g, tf_book* b, uint64_t n);

TF_API const char* tf_status_string(int32_t status);

#ifdef __cplusplus
}
#endif
#endif /* TICKFORGE_C_API_H */
