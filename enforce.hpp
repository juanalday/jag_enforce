#ifndef JAG_ENFORCE_HPP
#define JAG_ENFORCE_HPP

#include <concepts>
#include <format>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

// Two-level macro indirection: the outer macro forces full expansion of its
// argument before the inner macro stringizes it.  Without this indirection,
// ENFORCE_PP_STRINGIZE(SOME_MACRO) would produce "SOME_MACRO" instead of the
// macro's expanded text.
#define ENFORCE_PP_STRINGIZE(text)   ENFORCE_PP_STRINGIZE_I(text)
#define ENFORCE_PP_STRINGIZE_I(...)  #__VA_ARGS__

// ENFORCE(exp)           — asserts that exp is truthy at runtime.
//                          On failure throws std::runtime_error whose message
//                          contains the stringized source expression.
// ENFORCE(exp, fmt, ...) — same, but prepends a std::format(fmt, ...) string
//                          supplied by the caller before the expression text.
//
// __VA_OPT__ ensures the user-supplied format lambda is emitted only when
// format arguments are present.  Without it, ENFORCE(x) would expand to a
// std::format() call with no format-string argument, which is ill-formed.
#define ENFORCE(exp, ...)                                                           \
    jag::enforce(exp                                                                \
        __VA_OPT__(, [&]() -> std::string { return std::format(__VA_ARGS__); })     \
        , []() -> std::string {                                                     \
            return "Expression \"" ENFORCE_PP_STRINGIZE(exp) "\" failed";           \
        })

namespace jag {
namespace detail {

// ===================================================================
// Concepts
//
// Each concept classifies one role a callable argument may play inside
// enforce().  The concepts are intentionally narrow: no callable should
// accidentally satisfy more than one role, which keeps the dispatch
// predictable and the error messages meaningful.
// ===================================================================

// validator<F> / validator<F, T>
//   A callable that decides whether the enforced value is acceptable.
//   The return type must be exactly bool — not merely something convertible
//   to bool — so that int- or pointer-returning lambdas never silently match.
//   sizeof...(T) <= 1 restricts the pack to zero (nullary) or one (unary)
//   argument:
//     nullary — [&]{ return flag; }         captures external state, ignores value
//     unary   — [](int v){ return v > 0; }  receives and inspects the value
template <class F, class... T>
concept validator =
    (sizeof...(T) <= 1) &&
    std::invocable<F, T...> &&
    std::same_as<std::invoke_result_t<F, T...>, bool>;

// raiser<F>
//   A callable that receives the assembled error message and throws a custom
//   exception type in place of the default std::runtime_error.  Must return
//   void; a non-void return type could be mistaken for an appender.
template <class F>
concept raiser =
    std::invocable<F, std::string> &&
    std::same_as<std::invoke_result_t<F, std::string>, void>;

// value_producer<T>
//   A nullary callable used as the *first* argument to enforce() instead of a
//   plain value.  enforce() calls it to obtain the value, then validates that
//   value against the remaining arguments.
//   Void-returning callables are excluded because there is nothing to validate.
template <class T>
concept value_producer =
    std::invocable<T> &&
    !std::same_as<std::invoke_result_t<T>, void>;

// appendable<F> / appendable<F, T>
//   A callable that contributes a fragment to the human-readable error message.
//   Both std::string and std::string_view are accepted as return types so
//   callers can return string literals ([] { return "bad value"; }) without
//   incurring a heap allocation.
template <class F, class... Args>
concept appendable =
    std::invocable<F, Args...> &&
    (std::convertible_to<std::invoke_result_t<F, Args...>, std::string> ||
     std::convertible_to<std::invoke_result_t<F, Args...>, std::string_view>);

// ===================================================================
// Validation
// ===================================================================

// Dispatches a single callable f as a validator against value t.
//
// Priority order:
//   1. Nullary validator — f() — so that a capturing lambda such as
//      [&]{ return flag; } is never accidentally called with the value.
//   2. Unary validator   — f(t) — receives and evaluates the value.
//   3. Neither           — returns true, letting appenders and raisers pass
//      through this step without affecting the validation result.
template <typename T, typename F>
constexpr bool validate_impl(T&& t, F&& f) {
    if constexpr (validator<F>) {
        return f();                               // nullary: ignores the value
    } else if constexpr (validator<F, T>) {
        return f(std::forward<T>(t));             // unary: receives the value
    }
    return true;                                  // not a validator — transparent
}

// Validates t against every callable in args.
//
// Compile-time dispatch:
//   The outer fold (... || ...) is evaluated at compile time and asks whether
//   args contains at least one validator.  If not, the entire validation branch
//   is skipped and we fall through to the truthiness check below — avoiding a
//   redundant AND-fold over a set that contains no validators.
//
//   The inner fold (... && ...) is a left-to-right short-circuit AND: once any
//   validator returns false, remaining validators are not evaluated.
//
// Truthiness fallback (no validators in args):
//   Array references (const char (&)[N]) are always non-null; they are treated
//   as unconditionally valid so that ENFORCE("invariant text") never throws.
//   Anything else that converts to bool uses its natural truthiness:
//     non-null pointer → true, non-zero integer → true, …
//   Types that are not bool-convertible (std::string, std::vector, …) return
//   false, forcing the caller to provide an explicit validator.
template <typename T, typename... Args>
constexpr bool validate(T&& t, Args&&... args) {
    if constexpr ((... || (validator<Args, T> || validator<Args>))) {
        return (... && validate_impl(std::forward<T>(t), std::forward<Args>(args)));
    }

    // No validators supplied — fall back to the intrinsic truthiness of t.
    if constexpr (std::is_array_v<std::remove_reference_t<T>>) {
        return true;                              // array ref (e.g. string literal) is never null
    } else if constexpr (std::convertible_to<T, bool>) {
        return static_cast<bool>(t);              // pointer / integer / bool
    }
    return false;                                 // not bool-convertible and no validator → always fails
}

// Convenience negation so call sites read naturally: "if (wrong(t, ...)) throw".
template <typename T, typename... Args>
constexpr bool wrong(T&& t, Args&&... args) {
    return !validate(std::forward<T>(t), std::forward<Args>(args)...);
}

// ===================================================================
// Error message assembly
// ===================================================================

// Appends the text produced by one callable to the growing error buffer.
// Nullary appenders are tried before unary ones (same priority rule as
// validate_impl).  Callables that are not appendable — validators, raisers —
// contribute nothing and are silently ignored.
template <typename T, typename F>
void append_impl(std::string& buffer, T&& t, F&& f) {
    if constexpr (appendable<F>) {
        buffer += f();                            // nullary appender
    } else if constexpr (appendable<F, T>) {
        buffer += f(std::forward<T>(t));          // unary appender — receives the value
    }
}

// Concatenates the text produced by every appendable in args into one string.
// The unary comma fold (,...) visits every element left-to-right, so message
// fragments appear in the order they were passed to enforce().
// If no callable contributes any text, a generic fallback string is returned.
template <typename T, typename... Args>
std::string append(T&& t, Args&&... args) {
    std::string buffer;
    (..., append_impl(buffer, std::forward<T>(t), std::forward<Args>(args)));
    if (buffer.empty())
        buffer = "Expression has failed";
    return buffer;
}

// ===================================================================
// Exception raising
// ===================================================================

// Calls f(msg) if and only if F satisfies the raiser concept.
// Non-raisers (validators, appenders) are silently skipped, so the same
// argument pack can hold all callable roles without explicit type dispatch.
template <typename F>
void raise_impl(std::string const& msg, F&& f) {
    if constexpr (raiser<F>)
        f(msg);
}

// Walks every callable in args looking for raisers.  The first raiser to throw
// unwinds the stack with its custom exception type (e.g. std::logic_error).
// If no raiser throws — because none were provided, or one returned normally
// instead of throwing — the fold falls through to the unconditional
// std::runtime_error below, guaranteeing that enforce() always throws on failure.
template <typename... Args>
[[noreturn]] void raise(std::string const& msg, Args&&... args) {
    (..., raise_impl(msg, std::forward<Args>(args)));
    throw std::runtime_error(msg);                // fallback if no custom raiser threw
}

} // namespace detail

// ===================================================================
// Public API
// ===================================================================

// enforce(t, callables...)
//
//   Validates t — or the value produced by calling t if t is a value_producer
//   — against the supplied callables, throwing on failure.  Returns t (or the
//   produced value) with its original value category preserved, so callers can
//   chain directly or bind references:
//
//     int& x = enforce([&]() -> int& { return member; }, validator);
//     enforce(ptr)->method();
//     auto val = enforce([] { return compute(); }, [](int v){ return v >= 0; });
//
// Callable roles — resolved entirely at compile time, combinable freely:
//   validator  — bool() or bool(T)    decides pass / fail
//   appendable — string() or string(T) contributes to the error message
//   raiser     — void(string)          throws a custom exception type
//
// If validation fails and no raiser throws a custom exception, enforce()
// throws std::runtime_error with the concatenated appender output (or a
// generic "Expression has failed" message if no appenders were provided).
template <typename T, typename... Args>
constexpr decltype(auto) enforce(T&& t, Args&&... args) {
    if constexpr (detail::value_producer<T>) {
        // t is a nullary callable (value producer) — call it to obtain the value,
        // then enforce the value rather than the callable itself.
        //
        // decltype(auto) preserves the producer's exact return type including any
        // reference qualifier: a producer returning int& yields int& here, not a copy.
        decltype(auto) value = std::invoke(std::forward<T>(t));

        if constexpr (std::is_reference_v<decltype(value)>) {
            // The producer returned a reference — recurse so that validators
            // receive the correct reference type (e.g. int& rather than int).
            return enforce(std::forward<decltype(value)>(value), std::forward<Args>(args)...);
        } else {
            // The producer returned by value — enforce the local copy, then return it.
            // We cannot write "return enforce(value, ...)" because decltype(auto) would
            // deduce the return as int& (an lvalue reference to a local), which dangles
            // after the function returns.  Enforcing in-place and returning by value is safe.
            enforce(value, std::forward<Args>(args)...);
            return value;
        }
    } else {
        // t is a plain value (not a callable) — enforce it directly.
        if (detail::wrong(std::forward<T>(t), std::forward<Args>(args)...)) {
            std::string const msg = detail::append(std::forward<T>(t), std::forward<Args>(args)...);
            detail::raise(msg, std::forward<Args>(args)...);
        }
        // std::forward preserves the original value category so the caller
        // receives T& for lvalues, T const& for const lvalues, and T&& for rvalues.
        return std::forward<T>(t);
    }
}

} // namespace jag

#endif // JAG_ENFORCE_HPP
