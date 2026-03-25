# jag::enforce

A modern C++23 header-only runtime enforcement library.  Pass a value and
optional callables — validators, error formatters, and custom raisers — and
`enforce` either returns the value unchanged or throws.

Inspired by the original *Enforcements* technique by Petru Marginean and
Andrei Alexandrescu
([Dr. Dobb's, June 2003](https://web.archive.org/web/20241123174936/http://www.drdobbs.com/enforcements/184403864)),
but redesigned from scratch to eliminate eager evaluation and to support
C++20 concepts-based dispatch.

---

## Background: the original Enforcements (Alexandrescu & Marginean, 2003)

The article introduced a deceptively simple observation: every exception throw
follows the same pattern, so it should be abstractable:

```cpp
// before — repeated everywhere
if (!ptr) throw std::runtime_error("null pointer");

// after — Enforce as a filter function that returns its argument
Widget* p = Enforce(MakeWidget(), "null widget");
cout << p->ToString();   // p is guaranteed non-null here
```

The key insight is **filtering semantics**: `Enforce` returns the tested value
unchanged, so it composes naturally in expressions instead of breaking the flow
with an `if` block.

The article then evolved the idea into a policy-based `Enforcer` class with
`operator*` and a chained `operator()` for lazy message construction:

```cpp
// Final form from the article
ENFORCE(pWidget)("Widget number ")(n)(" is null and it shouldn't be!");
```

The `ENFORCE` macro expands to a `MakeEnforcer` call wrapped in `operator*`:

```cpp
#define STRINGIZE(x) STRINGIZE_HELPER(x)
#define STRINGIZE_HELPER(x) #x
#define ENFORCE(exp) \
    *MakeEnforcer<DefaultPredicate, DefaultRaiser>( \
        (exp), "Expression '" #exp "' failed in '" \
        __FILE__ "', line: " STRINGIZE(__LINE__))
```

The `Enforcer` class stores whether validation failed at construction time
(`locus_` is non-null only on failure), so subsequent `operator()` calls are
no-ops on the success path:

```cpp
template<typename Ref, typename P, typename R>
class Enforcer {
public:
    Enforcer(Ref t, const char* locus)
        : t_(t), locus_(P::Wrong(t) ? locus : 0) {}

    Ref operator*() const {
        if (locus_) R::Throw(t_, msg_, locus_);
        return t_;
    }
    template <class MsgType>
    Enforcer& operator()(const MsgType& msg) {
        if (locus_) {
            std::ostringstream ss;
            ss << msg;
            msg_ += ss.str();   // only runs on failure
        }
        return *this;
    }
private:
    Ref t_;
    std::string msg_;
    const char* const locus_;
};
```

The two policies are plain structs with static members, giving compile-time
configurability with zero overhead:

```cpp
struct DefaultPredicate {
    template <class T>
    static bool Wrong(const T& obj) { return !obj; }
};
struct DefaultRaiser {
    template <class T>
    static void Throw(const T&, const std::string& msg, const char* locus) {
        throw std::runtime_error(msg + '\n' + locus);
    }
};
```

Custom policies enable per-API error conventions with a single macro:

```cpp
// Check for -1 (POSIX / Win32 HANDLE conventions)
struct HandlePredicate {
    static bool Wrong(long handle) { return handle == -1; }
};
#define HANDLE_ENFORCE(exp) \
    *MakeEnforcer<HandlePredicate, DefaultRaiser>((exp), ...)

const int bytesRead = HANDLE_ENFORCE(_read(file, buffer, buflen));
```

### What the article got right

- **Filtering semantics** — returning the validated value so enforcement
  composes in expressions.
- **Lazy message building** — `operator()` no-ops on the success path; the
  `ostringstream` work only happens when an exception will actually be thrown.
- **Policy-based design** — predicate and raiser policies separate the
  *what-is-wrong* question from the *how-to-report-it* question, at zero
  runtime cost.
- **One macro per error convention** — `ENFORCE`, `HANDLE_ENFORCE`,
  `COM_ENFORCE`, `CLIB_ENFORCE`, `WAPI_ENFORCE` each encode a different
  API's error contract.

### The remaining problems

**Argument evaluation is still eager.**
In `ENFORCE(exp)(arg1)(arg2)`, the expressions `arg1` and `arg2` are
evaluated by C++ before `operator()` is called — even on the success path.
Only the `ostringstream` formatting is skipped.  A call to an expensive
diagnostic function still runs even when there is no error.

**Predicate is fixed at macro-definition time.**
The policy is baked into the macro.  Choosing a different predicate for a
specific call site requires a new macro.

**`operator()` accepts only streamable arguments.**
The chaining mechanism uses `std::ostringstream`, which means the message
fragments must satisfy `operator<<`.  Structured data needs manual
serialisation.

**C++03 limitations.**
Template policies, manual `const`/non-`const` overloads, and the two-level
`STRINGIZE` trick for `__LINE__` were the best tools available in 2003.
C++20/23 renders all of them unnecessary.

---

## How jag::enforce addresses these problems

`jag::enforce` accepts the value and a set of **lazy callables**.  Arguments
are lambdas, so their bodies execute **only when called** — which only happens
when validation has already failed:

```cpp
jag::enforce(variable,
    [](auto const& v) -> bool { return v != "hello"; },        // validator
    [](auto const& v) { return expensive_error_message(v); },  // only called on failure
    [](std::string msg) { throw MyException(msg); }            // custom raiser
);
```

Each callable is categorised by its signature at **compile time** using
C++20 concepts, so there is no virtual dispatch, no type erasure, and no
runtime overhead on the success path.

---

## Requirements

- C++23 compiler (MSVC 19.35+, GCC 13+, Clang 17+)
- Header-only — just `#include "enforce.hpp"`

---

## Quick start

```cpp
#include "enforce.hpp"

// 1. Null check — throws std::runtime_error if p is null.
int* p = get_pointer();
jag::enforce(p);

// 2. Pass-through — returns the value so it can be used immediately.
auto& conn = jag::enforce(open_connection());

// 3. Custom message — only evaluated when the check fails.
jag::enforce(config.port,
    [](int port) -> bool { return port > 0 && port < 65536; },
    [](int port) { return std::format("invalid port: {}", port); }
);
```

---

## First argument: the value

The first argument is the **value** (or value producer) to enforce.  It is
never treated as a callable for dispatch purposes, so there is no ambiguity
between a `const char*` value and a lambda returning `const char*`.

### Direct value

Any expression whose result should be validated:

```cpp
jag::enforce(ptr);             // pointer — truthy if non-null
jag::enforce(handle > 0);      // bool expression
jag::enforce(foo());           // return value of foo()
```

### Value producer

A **nullary callable that returns a non-void type** is recognised as a
`value_producer`.  `enforce` calls it first, then validates the result.
Validators and appenders see the *produced value*, not the callable:

```cpp
// Produces an int, then validates it.
jag::enforce([] { return compute(); },
    [](int v) -> bool { return v >= 0; });

// Produces a reference — forwarded through enforce() so callers can bind it.
int& x = jag::enforce([&]() -> int& { return member; },
    [](int& v) -> bool { return v != sentinel; });
```

---

## Subsequent arguments: callables

All arguments after the first are callables.  Each is inspected at compile
time and assigned one of three roles based on its signature.  Roles can be
combined freely in any order:

```cpp
jag::enforce(value, validator, appender, raiser);   // all three
jag::enforce(value, appender, validator);            // order does not matter
jag::enforce(value, validator, validator, appender); // multiple validators
```

### Callable dispatch

| Concept | Signature(s) | Role |
|---|---|---|
| `validator` | `() -> bool` or `(T) -> bool` | Decides pass / fail |
| `appendable` | `() -> string` or `(T) -> string` | Contributes to the error message |
| `raiser` | `(std::string) -> void` | Throws a custom exception |

`string` means any type convertible to `std::string` or `std::string_view`,
including `const char*` literals.

A callable that matches none of the concepts is silently ignored.

### Defaults when a role is absent

| Missing role | Default behaviour |
|---|---|
| No `validator` | Value is tested via `static_cast<bool>` |
| No `appendable` | Error message is `"Expression has failed"` |
| No `raiser` | `std::runtime_error` is thrown |

### Nullary vs. unary callables

Every role supports two call forms:

- **Nullary** — `[]() { … }` — ignores the value; useful for capturing
  external context or returning a fixed message.
- **Unary** — `[](T v) { … }` — receives the enforced value, enabling
  validators and messages that reference it.

The nullary form is always tried before the unary form, so a capturing
lambda is never accidentally called with the value as an argument.

---

## Return value and value categories

`enforce` returns the original value with its **value category preserved**:

| Input | Return type |
|---|---|
| `T&` (lvalue) | `T&` — same object, no copy |
| `T const&` | `T const&` — same object, const preserved |
| `T&&` (rvalue / xvalue) | `T&&` — no copy or move performed inside enforce |
| value producer `→ T` | `T` — produced value returned by value |
| value producer `→ T&` | `T&` — produced reference forwarded unchanged |

This makes the following patterns safe and copy-free:

```cpp
// Bind a reference returned from a producer.
int& x = jag::enforce([&]() -> int& { return map.at(key); });

// Chain directly without a named variable.
jag::enforce(get_ptr())->method();

// Non-copyable types — enforce introduces no copies or moves.
std::unique_ptr<Foo> p = jag::enforce(std::move(owned_ptr));
```

---

## Usage examples

### Bool conversion (default)

```cpp
jag::enforce(ptr);           // null → throws
jag::enforce(count > 0);     // false → throws
jag::enforce(handle);        // zero/null → throws
```

### Custom validator

```cpp
// Unary — receives the value.
jag::enforce(config.port,
    [](int p) -> bool { return p > 0 && p < 65536; });

// Nullary — captures state, ignores the value.
bool system_ready = check_system();
jag::enforce(request, [&] { return system_ready; });
```

### Multiple validators (short-circuit AND)

Validators are evaluated left-to-right; once one returns `false`, later
validators are not called.

```cpp
jag::enforce(value,
    [](int v) -> bool { return v > 0; },      // must be positive
    [](int v) -> bool { return v % 2 == 0; }  // must be even (skipped if first fails)
);
```

### Custom error message

```cpp
// Fixed message (nullary appender).
jag::enforce(result, [] { return "operation produced an invalid result"; });

// Value-aware message (unary appender).
jag::enforce(code,
    [](int c) -> bool { return c == 0; },
    [](int c) { return std::format("expected exit code 0, got {}", c); });
```

Multiple appenders are concatenated in the order they appear:

```cpp
jag::enforce(value,
    []        { return "context: "; },
    [](int v) { return std::format("value was {}", v); }
);
// error message → "context: value was 42"
```

### Custom exception type

```cpp
jag::enforce(ptr, [](std::string msg) {
    throw std::invalid_argument(msg);
});
```

The raiser receives the fully assembled error message.  If it returns
without throwing, `std::runtime_error` is thrown as a fallback guarantee.

### Combining all three roles

```cpp
jag::enforce(response.status,
    [](int code) -> bool  { return code >= 200 && code < 300; }, // validator
    [](int code)          { return std::format("HTTP {}", code); }, // appender
    [](std::string msg)   { throw HttpError(msg); }               // raiser
);
```

### Mutating through a reference

When a value producer returns a reference, validators receive that same
reference and may mutate through it:

```cpp
int original = 5;
int& ref = jag::enforce(
    [&]()      -> int& { return original; },
    [](int& v) -> bool { v = normalise(v); return v >= 0; }  // mutates original
);
```

---

## The ENFORCE macro

The `ENFORCE` macro stringifies the source expression and appends it to the
error message automatically.  It accepts an optional `std::format` string:

```cpp
ENFORCE(x > 0);
// on failure → "Expression \"x > 0\" failed"

ENFORCE(x > 0, "expected positive, got {}", x);
// on failure → "expected positive, got -3Expression \"x > 0\" failed"
```

The two message fragments are concatenated directly; include a separator in
the format string if you need one:

```cpp
ENFORCE(x > 0, "expected positive, got {}. ", x);
// → "expected positive, got -3. Expression \"x > 0\" failed"
```

---

## Execution flow

For every `enforce()` call the following steps run in order:

1. **Produce** — if the first argument is a `value_producer`, call it to
   obtain the value; otherwise use the first argument directly.

2. **Validate** — call each `validator` left-to-right, short-circuiting on
   the first `false`.  If no validator is present, test the value via
   `static_cast<bool>`.  Array references (e.g. string literals passed
   directly) are always treated as valid.

3. **Pass-through** — if validation succeeds, return the value immediately
   with its original value category.  Steps 4 and 5 do not run.

4. **Build message** — call each `appendable` in order and concatenate their
   outputs.  If no appender contributed text, use `"Expression has failed"`.

5. **Raise** — call each `raiser` in order with the assembled message.  If
   any raiser throws, the exception propagates.  If none throw (or none
   exist), throw `std::runtime_error(message)`.

---

## Comparison with the original

| | Alexandrescu & Marginean (2003) | jag::enforce (C++23) |
|---|---|---|
| Lazy validation | `operator*` checks `locus_` | Concept dispatch; lambdas not called on success |
| Lazy message building | `operator()` no-ops if no error | Appender lambdas only called on failure |
| Argument evaluation | **Eager** — args to `operator()` always evaluated | **Lazy** — lambda bodies only evaluated on failure |
| Predicate | Policy template parameter, fixed per macro | Any `() -> bool` or `(T) -> bool` lambda, per call |
| Error convention | One macro per convention (`HANDLE_ENFORCE`, etc.) | One `enforce()` call with a custom validator lambda |
| Message formatting | `std::ostringstream` + `operator<<` | `std::format` or any string-returning lambda |
| Custom exception | Raiser policy baked into macro | Any `(std::string) -> void` lambda |
| Value categories | `const T&` / `T&` overloads | Full perfect forwarding; lvalue/rvalue/ref all preserved |
| `__FILE__`/`__LINE__` | Captured by macro into error message | Available via `ENFORCE` macro; not required for plain `enforce()` |
| Language standard | C++03 | C++23 |

---

## Design notes

**Why exactly `bool` for validators, not just bool-convertible?**
A weaker constraint would let lambdas returning `int`, `std::string`, or a
pointer accidentally satisfy the `validator` concept.  Requiring exactly
`bool` keeps the dispatch unambiguous.

**Why is the nullary form tried before the unary form?**
A capturing nullary lambda such as `[&]{ return flag; }` is also invocable
with any argument (the argument is simply discarded by some compilers).
Checking nullary first ensures such lambdas are never accidentally called
with the enforced value.

**Why does `appendable` accept `std::string_view` as well as `std::string`?**
Returning a string literal from an appender (`[] { return "bad value"; }`)
yields `const char*`, which is convertible to both.  Accepting `string_view`
avoids a heap allocation for pure-literal messages.

**Why does `raise` always throw after visiting the raisers?**
A raiser *should* throw, but the language has no mechanism to enforce that.
The unconditional `std::runtime_error` fallback at the end of `raise`
guarantees that `enforce` never returns normally after a failed validation,
regardless of what a custom raiser does.

---

## Building

```bash
cmake -B build
cmake --build build
```

### Running tests

```bash
cmake -B build -DENABLE_TEST=ON
cmake --build build
./build/Debug/enforce_test   # Windows
./build/enforce_test          # Linux / macOS
```

Tests use [Google Test](https://github.com/google/googletest), fetched
automatically via CMake `FetchContent`.

---

## License

See `LICENSE` for details.
