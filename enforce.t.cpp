#include "enforce.hpp"

#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

using jag::enforce;
using namespace jag::detail;
using std::string_view_literals::operator""sv;

namespace {

// noncopyable — throws from every copy and move special member.
// Used as a canary throughout the perfect-forwarding tests: if enforce()
// ever copies or moves the value internally, the test will catch a runtime_error.
// operator bool() returns true so the value always passes the truthiness check.
struct noncopyable {
    noncopyable() = default;
    noncopyable(noncopyable const&)            { throw std::runtime_error("copy ctor"); }
    noncopyable& operator=(noncopyable const&) { throw std::runtime_error("copy assign"); }
    noncopyable(noncopyable&&)                 { throw std::runtime_error("move ctor"); }
    noncopyable& operator=(noncopyable&&)      { throw std::runtime_error("move assign"); }
    operator bool() const { return true; }
};

} // anonymous namespace

// ===================================================================
// Concept verification
//
// These tests are purely compile-time (static_assert) or trivially runtime
// (EXPECT_TRUE/FALSE on concept values).  They pin the exact boundaries of
// each concept so that accidental widening or narrowing is caught immediately.
// ===================================================================

TEST(concepts, validator) {
    // --- positive cases ---

    // Unary validator: takes the enforced value by copy, returns bool.
    static_assert(validator<decltype([](int) -> bool { return true; }), int>);
    // Nullary validator: captures external state, ignores the value.
    static_assert(validator<decltype([]() -> bool { return true; })>);

    // --- negative cases ---

    // Returns int instead of bool — convertible to bool, but the concept requires
    // exactly bool to prevent accidental matches by comparison-result lambdas.
    static_assert(!validator<decltype([](int) -> int { return 1; }), int>);
    // Returns void — cannot be used as a truth value.
    static_assert(!validator<decltype([](int) {}), int>);
    // Returns std::string — not a bool, even though it could be evaluated as truthy.
    static_assert(!validator<decltype([]() { return std::string{}; })>);
}

TEST(concepts, raiser) {
    // --- positive cases ---

    // Canonical raiser: takes std::string (the assembled error message), returns void.
    static_assert(raiser<decltype([](std::string) {})>);

    // --- negative cases ---

    // Returns non-void — would be misidentified as an appender by the dispatch logic.
    static_assert(!raiser<decltype([](std::string) { return 1; })>);
    // Takes int instead of std::string — wrong argument type.
    static_assert(!raiser<decltype([](int) {})>);
}

TEST(concepts, value_producer) {
    // --- positive cases ---

    // Returns int — the archetypal value producer.
    static_assert(value_producer<decltype([] { return 42; })>);
    // Returns std::string — non-primitive types are fine.
    static_assert(value_producer<decltype([] { return std::string{"hi"}; })>);

    // --- negative cases ---

    // Returns void — there is no produced value to enforce.
    static_assert(!value_producer<decltype([] {})>);
    // int is not invocable at all, let alone a value producer.
    static_assert(!value_producer<int>);
}

// ===================================================================
// Return type verification
//
// enforce() is designed to preserve the exact value category of its argument
// so that callers can take references, bind rvalue references, or assign — all
// without an extra copy.  These static_asserts nail down the expected return
// types for the four standard value categories.
// ===================================================================

TEST(enforce, return_types_direct_value) {
    // lvalue      → T&        (identity — the same object is returned)
    static_assert(std::is_same_v<decltype(enforce(std::declval<int&>())),       int&>);
    // const lvalue → T const& (identity — const preserved)
    static_assert(std::is_same_v<decltype(enforce(std::declval<int const&>())), int const&>);
    // rvalue / prvalue → T&&  (rvalue reference — no materialization inside enforce)
    static_assert(std::is_same_v<decltype(enforce(std::declval<int>())),        int&&>);

    // Adding callables must not change the return type — the value still flows through.
    auto pass = []() -> bool { return true; };
    static_assert(std::is_same_v<decltype(enforce(std::declval<int&>(), pass)), int&>);
}

// ===================================================================
// Perfect forwarding — zero copies, identity preservation
//
// noncopyable throws from every copy/move special member, so any unintended
// materialisation inside enforce() will cause a test failure at runtime.
// ===================================================================

TEST(enforce, perfect_forwarding) {
    // lvalue: enforce must return a reference to the same object, not a copy.
    {
        noncopyable x;
        noncopyable& ref = enforce(x);
        EXPECT_EQ(std::addressof(ref), std::addressof(x));
    }

    // const lvalue: same identity guarantee, const preserved.
    {
        noncopyable const cx;
        noncopyable const& ref = enforce(cx);
        EXPECT_EQ(std::addressof(ref), std::addressof(cx));
    }

    // xvalue (std::move): enforce returns T&&; the rvalue reference still
    // names the original object — no move construction is performed.
    {
        noncopyable x;
        noncopyable&& ref = enforce(std::move(x));
        EXPECT_EQ(std::addressof(ref), std::addressof(x));
    }

    // prvalue: a temporary is created once at the call site; enforce must not
    // trigger an additional move when returning it.
    {
        EXPECT_NO_THROW(enforce(noncopyable()));
        [[maybe_unused]] noncopyable&& ref = enforce(noncopyable());
    }

    // Forwarding an lvalue: no copy should occur inside enforce().
    {
        noncopyable value;
        EXPECT_NO_THROW(enforce(value));
        [[maybe_unused]] decltype(auto) ref = enforce(value);
    }

    // Forwarding a const lvalue: same guarantee, const qualified.
    {
        noncopyable const cvalue;
        EXPECT_NO_THROW(enforce(cvalue));
        [[maybe_unused]] decltype(auto) cref = enforce(cvalue);
    }

    // The copy happens at the *call site*, not inside enforce().
    // "auto t = enforce(noncopyable())" copies the returned noncopyable&&
    // into t — that copy is what throws, not anything enforce() does.
    {
        EXPECT_THROW(([] { auto t = enforce(noncopyable()); }()), std::runtime_error);
    }
}

// ===================================================================
// Default behaviour — bool conversion, no callables
// ===================================================================

TEST(enforce, bool_validation) {
    EXPECT_NO_THROW(enforce(true));
    EXPECT_TRUE(enforce(true));               // enforce returns the value itself
    EXPECT_THROW(enforce(false), std::runtime_error);
}

TEST(enforce, truthiness) {
    // Non-zero integers are truthy; zero throws.
    {
        EXPECT_NO_THROW(enforce(1));
        EXPECT_NO_THROW(enforce(42));
        EXPECT_NO_THROW(enforce(-3));
        EXPECT_THROW(enforce(0), std::runtime_error);
    }

    // Non-null pointers pass; null pointers throw.
    {
        int value = 10;
        int* p      = &value;
        int* null_p = nullptr;

        EXPECT_EQ(enforce(p), &value);
        EXPECT_THROW(enforce(null_p), std::runtime_error);
    }
}

TEST(enforce, non_bool_convertible_always_fails) {
    // std::string has no conversion to bool, so without a validator enforce()
    // falls through to the "return false" branch and always throws.
    using namespace std::literals;
    EXPECT_THROW(enforce("hello"s),       std::runtime_error);
    EXPECT_THROW(enforce(std::string()),  std::runtime_error);

    // std::string_view is likewise not bool-convertible.
    EXPECT_THROW(enforce("hi"sv), std::runtime_error);

    // String literals have type const char (&)[N] — an array reference.
    // The array branch in validate() unconditionally returns true so that
    // ENFORCE("assertion text") does not always throw.
    EXPECT_NO_THROW(enforce("non-empty"));
    EXPECT_NO_THROW(enforce(""));           // array ref is non-null even for ""

    // std::vector is not bool-convertible — always fails without a validator.
    EXPECT_THROW(enforce(std::vector<int>{}), std::runtime_error);
}

TEST(enforce, default_error_message) {
    // When no appender is provided the error message falls back to the
    // generic "Expression has failed" string produced by detail::append().
    try {
        enforce(false);
        FAIL() << "Expected std::runtime_error";
    } catch (std::runtime_error const& e) {
        EXPECT_STREQ(e.what(), "Expression has failed");
    }
}

// ===================================================================
// Value producers
// ===================================================================

TEST(enforce, value_producer_basic) {
    // A bool-returning producer follows the same truthiness rules as a plain bool.
    EXPECT_NO_THROW(enforce([] { return true; }));
    EXPECT_THROW(enforce([] { return false; }), std::runtime_error);

    // The produced value is returned by enforce() so it can be used directly.
    EXPECT_EQ(enforce([] { return 42; }), 42);
}

TEST(enforce, value_producer_pointer) {
    // A null pointer produced by a lambda triggers the same null check as a
    // direct null pointer argument.
    int* p = nullptr;
    EXPECT_THROW(enforce([&] { return p; }), std::runtime_error);

    // A non-null pointer passes and is returned unchanged.
    int x   = 10;
    int* q  = &x;
    EXPECT_EQ(enforce([&] { return q; }), &x);
}

TEST(enforce, value_producer_with_validator) {
    // The validator receives the produced value, not the lambda itself.
    EXPECT_NO_THROW(enforce([] { return  5; }, [](int v) -> bool { return v > 0; }));
    EXPECT_THROW(   enforce([] { return -1; }, [](int v) -> bool { return v > 0; }),
                    std::runtime_error);
}

TEST(enforce, value_producer_with_appender) {
    // An appender attached to a value producer appears in the error message
    // just as it would for a plain value.
    try {
        enforce([] { return false; }, [] { return "produced value was false"; });
        FAIL() << "Expected std::runtime_error";
    } catch (std::runtime_error const& e) {
        EXPECT_STREQ(e.what(), "produced value was false");
    }
}

TEST(enforce, value_producer_mutation_by_value) {
    // When the producer returns by value, the validator receives a copy.
    // Mutating that copy does not affect the value returned by enforce().
    EXPECT_EQ(enforce([] { return 5; }, [](int v) -> bool { v = 7; return true; }), 5);
}

TEST(enforce, value_producer_mutation_by_reference) {
    // When the producer returns by reference, the validator receives the same
    // reference and can mutate the original object through it.
    int original = 5;
    int& ref = enforce(
        [&]()      -> int& { return original; },
        [](int& v) -> bool { v = 7; return true; }  // mutates original through the ref
    );
    EXPECT_EQ(original, 7);
    EXPECT_EQ(std::addressof(ref), std::addressof(original));
}

TEST(enforce, value_producer_with_all_roles) {
    // All three callable roles (validator, appender, raiser) can be combined in
    // a single enforce() call.  The appender message is assembled first, then
    // passed to the raiser which throws the custom exception.
    struct custom_error : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    try {
        enforce(
            [] { return -5; },
            [](int v) -> bool  { return false; },                                    // validator
            [](int v)          { return std::format("produced {} which is not positive", v); }, // appender
            [](std::string msg){ throw custom_error(msg); }                          // raiser
        );
        FAIL() << "Expected custom_error";
    } catch (custom_error const& e) {
        EXPECT_STREQ(e.what(), "produced -5 which is not positive");
    }
}

// ===================================================================
// Validators
// ===================================================================

TEST(enforce, validator_by_category_of_argument) {
    // Nullary validator — the value is irrelevant; the validator checks only its
    // own captured state.  Passing false as the value would fail the default
    // truthiness check, so if the nullary validator is not called the test throws.
    {
        EXPECT_NO_THROW(enforce(false, [] { return true; }));
    }

    // Unary validator receiving an lvalue: both by-copy and by-lvalue-ref forms
    // should be invoked; validCalls lets us verify that both actually ran.
    {
        int x          = 4;
        int validCalls = 0;
        EXPECT_NO_THROW(enforce(x,
            [&](int)   { ++validCalls; return true;      },  // by copy
            [&](int& v){ ++validCalls; return v == 4;    }   // by lvalue ref
        ));
        EXPECT_EQ(validCalls, 2);
    }

    // Unary validator receiving a const lvalue: const& form must bind correctly.
    {
        int const cx = 4;
        EXPECT_NO_THROW(enforce(cx, [](int const& v) -> bool { return v == 4; }));
        EXPECT_THROW(   enforce(cx, [](int const& v) -> bool { return v != 4; }),
                        std::runtime_error);
    }
}
