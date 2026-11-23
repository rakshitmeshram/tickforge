// Input-validation tests: malformed messages, corrupt message files, the
// per-order quantity cap, market-order id checks and the capacity invariant.
// Book-level scenarios run against BOTH books, like test_order_book.cpp.
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "test_framework.hpp"
#include "tickforge/message.hpp"
#include "tickforge/order_book.hpp"
#include "tickforge/reference_book.hpp"

using namespace tickforge;

namespace {

template <class Book>
Book make_book() {
    return Book(typename Book::Config{0, 1000, 1024});
}

int st(ExecReport r) {
    return static_cast<int>(r.status);
}
int st(Status s) {
    return static_cast<int>(s);
}

Msg raw_msg(std::uint8_t type, std::uint8_t side, std::uint8_t tif) {
    Msg m{};
    m.type = static_cast<MsgType>(type);
    m.side = static_cast<Side>(side);
    m.tif = static_cast<Tif>(tif);
    m.id = 1;
    m.price = 100;
    m.qty = 5;
    return m;
}

template <class Book>
void invalid_enum_fields_are_rejected() {
    auto b = make_book<Book>();
    CHECK_EQ(st(apply(b, raw_msg(0, 2, 0), NullSink{})), st(Status::RejectedInvalidMessage));    // side
    CHECK_EQ(st(apply(b, raw_msg(0, 255, 0), NullSink{})), st(Status::RejectedInvalidMessage));  // side
    CHECK_EQ(st(apply(b, raw_msg(0, 0, 3), NullSink{})), st(Status::RejectedInvalidMessage));    // tif
    CHECK_EQ(st(apply(b, raw_msg(4, 0, 0), NullSink{})), st(Status::RejectedInvalidMessage));    // type
    CHECK_EQ(b.order_count(), 0u);
    CHECK_EQ(b.best_bid(), kNoPrice);
    CHECK(apply(b, raw_msg(0, 0, 0), NullSink{}).ok());  // the valid message still works
    CHECK_EQ(b.best_bid(), 100);
}

template <class Book>
void market_order_rejects_live_id() {
    auto b = make_book<Book>();
    CHECK(b.limit(1, Side::Sell, 100, 5).ok());
    CHECK(b.limit(2, Side::Buy, 90, 5).ok());
    // Id 2 is resting: a market order may not reuse it.
    CHECK_EQ(st(b.market(2, Side::Buy, 3)), st(Status::RejectedDuplicateId));
    CHECK_EQ(st(b.market(~OrderId{0}, Side::Buy, 3)), st(Status::RejectedDuplicateId));
    CHECK_EQ(b.order_count(), 2u);
    // Once id 1 is fully consumed it is free again.
    CHECK_EQ(b.market(3, Side::Buy, 5).filled, 5);
    CHECK_EQ(b.market(1, Side::Sell, 1).filled, 1);
}

template <class Book>
void quantity_cap() {
    auto b = make_book<Book>();
    CHECK_EQ(st(b.limit(1, Side::Buy, 100, kMaxQty + 1)), st(Status::RejectedInvalidQty));
    CHECK_EQ(st(b.market(2, Side::Buy, kMaxQty + 1)), st(Status::RejectedInvalidQty));
    CHECK(b.limit(3, Side::Buy, 100, kMaxQty).ok());
    CHECK(b.limit(4, Side::Buy, 100, kMaxQty).ok());  // level total 2^31: no overflow
    CHECK_EQ(st(b.amend(3, 100, kMaxQty + 1)), st(Status::RejectedInvalidQty));
    CHECK_EQ(b.order_count(), 2u);
    const auto d = b.depth(Side::Buy, 1);
    REQUIRE(d.size() == 1);
    CHECK_EQ(d[0].qty, 2 * kMaxQty);
}

// A GTC remainder exists only after every crossing maker was consumed, which
// frees pool slots, so a full book can never hide fills behind
// RejectedCapacity.
template <class Book>
void capacity_rejection_never_hides_fills() {
    Book b(typename Book::Config{0, 100, 2});
    CHECK(b.limit(1, Side::Sell, 50, 3).ok());
    CHECK(b.limit(2, Side::Buy, 10, 1).ok());  // pool now full
    const auto r = b.limit(3, Side::Buy, 50, 5);
    CHECK(r.ok());
    CHECK_EQ(r.filled, 3);
    CHECK_EQ(r.resting, 2);  // the consumed maker's slot was reused
    // Non-crossing order into a full pool: rejected, nothing filled.
    const auto r2 = b.limit(4, Side::Buy, 5, 1);
    CHECK_EQ(st(r2), st(Status::RejectedCapacity));
    CHECK_EQ(r2.filled, 0);
}

// ------------------------------------------------------------ file reading

struct TempFile {
    std::string path;
    explicit TempFile(const char* name) : path((std::filesystem::temp_directory_path() / name).string()) {}
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

void write_raw(const std::string& path, const std::vector<unsigned char>& bytes) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    if (!bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
}

bool read_throws(const std::string& path) {
    try {
        static_cast<void>(read_messages(path));
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

std::vector<unsigned char> file_bytes(const std::string& path) {
    std::vector<unsigned char> v(static_cast<std::size_t>(std::filesystem::file_size(path)));
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f) {
        const std::size_t got = std::fread(v.data(), 1, v.size(), f);
        v.resize(got);
        std::fclose(f);
    }
    return v;
}

}  // namespace

#define BOTH(fn)             \
    TEST(fn##_fast) {        \
        fn<OrderBook>();     \
    }                        \
    TEST(fn##_reference) {   \
        fn<ReferenceBook>(); \
    }

BOTH(invalid_enum_fields_are_rejected)
BOTH(market_order_rejects_live_id)
BOTH(quantity_cap)
BOTH(capacity_rejection_never_hides_fills)

TEST(message_file_round_trip) {
    TempFile t("tickforge_test_roundtrip.bin");
    std::vector<Msg> msgs{raw_msg(0, 0, 0), raw_msg(1, 1, 0), raw_msg(2, 0, 0), raw_msg(3, 1, 0)};
    write_messages(t.path, msgs);
    const auto back = read_messages(t.path);
    REQUIRE(back.size() == msgs.size());
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        CHECK(back[i].type == msgs[i].type);
        CHECK(back[i].side == msgs[i].side);
        CHECK_EQ(back[i].id, msgs[i].id);
    }
    // Empty files of the right format are fine too.
    write_messages(t.path, {});
    CHECK(read_messages(t.path).empty());
}

TEST(message_file_rejects_corruption) {
    TempFile t("tickforge_test_corrupt.bin");
    write_messages(t.path, {raw_msg(0, 0, 0), raw_msg(0, 1, 0)});
    const auto good = file_bytes(t.path);
    REQUIRE(good.size() == 16 + 2 * sizeof(Msg));

    auto bytes = good;
    bytes.resize(bytes.size() - 1);  // truncated record
    write_raw(t.path, bytes);
    CHECK(read_throws(t.path));

    bytes = good;
    bytes.push_back(0);  // trailing garbage
    write_raw(t.path, bytes);
    CHECK(read_throws(t.path));

    bytes = good;
    for (int i = 8; i < 16; ++i) bytes[static_cast<std::size_t>(i)] = 0xFF;  // count = 2^64-1
    write_raw(t.path, bytes);
    CHECK(read_throws(t.path));  // must not attempt a huge allocation

    bytes = good;
    bytes[16 + sizeof(Msg) + 1] = 7;  // side byte of record 1
    write_raw(t.path, bytes);
    CHECK(read_throws(t.path));

    bytes = good;
    bytes[0] ^= 0xFF;  // bad magic
    write_raw(t.path, bytes);
    CHECK(read_throws(t.path));

    write_raw(t.path, {1, 2, 3});  // shorter than the header
    CHECK(read_throws(t.path));

    CHECK(read_throws(t.path + ".does-not-exist"));
}

TF_TEST_MAIN
