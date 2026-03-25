# jag::enforce

A modern C++23 header-only runtime enforcement library. Pass a value and optional callables -- validators, error formatters, and custom raisers -- and `enforce` either returns the value or throws.

Inspired by the original enforcements technique by Petru Marginean and Andrei Alexandrescu ([Dr. Dobb's article](http://www.drdobbs.com/enforcements/184403864)), but redesigned to avoid the eager-evaluation problem of the classic approach.

## The problem with classic enforcements

The classic `ENFORCE` macro evaluates all arguments unconditionally:

```cpp
ENFORCE(variable)("Variable is 'hello'. Here is a list of valid values: ")
                  (call_to_expensive_function(variable));
```

This expands to chained `operator()` calls that are always evaluated, even when enforcement passes. Side effects run, expensive computations execute, and cycles are wasted.

## How jag::enforce solves this

`jag::enforce` takes the value and a set of **lazy callables**. Nothing executes unless validation fails:

```cpp
jag::enforce(variable,
    [](auto const& v) -> bool { return v != "hello"; },           // validator
    [](auto const& v) { return expensive_error_message(v); },     // only called on failure
    [](std::string msg) { throw MyException(msg); }               // custom raiser
);
```

## Requirements

- C++23 compiler (MSVC 19.35+, GCC 13+, Clang 17+)
- Header-only: just include `enforce.hpp`

## Quick start

```cpp
#include "enforce.hpp"

int main() {
    int* p = get_pointer();
    jag::enforce(p);  // throws std::runtime_error if null

    auto& val = jag::enforce(get_value());  // pass-through on success
}
```

## Argument rules

```cpp
jag::enforce(value, callable1, callable2, ...);
//           ^^^^^  ^^^^^^^^^^^^^^^^^^^^^^^^^^
//           VALUE  CALLABLES (dispatched by concept)
```

The **first argument** is the **value** to enforce. It can be provided in two ways:

1. **Directly** -- a value or expression result:
   ```cpp
   jag::enforce(ptr);           // enforce a pointer
   jag::enforce(foo());         // enforce the return value of foo()
   jag::enforce(count > 0);     // enforce a boolean expression
   ```

2. **As a callable** (`value_producer`) -- a nullary callable that returns a non-void value. `enforce` calls it and enforces the result:
   ```cpp
   jag::enforce([&] { return foo(); });   // calls lambda, enforces the result
   ```

   A callable is detected as a `value_producer` if it is invocable with no arguments and returns a non-void type. The call happens before any validation, so validators and appenders see the produced value, not the callable.

All **subsequent arguments** are callables, dispatched at compile time by their signature into one of three roles: `validator`, `stringable`, or `raiser` (see [Callable dispatch](#callable-dispatch)). The first argument is excluded from this dispatch entirely, so there is no ambiguity between a value like `const char*` and a callable returning `const char*`.

## Usage

### Default predicate (bool conversion)

```cpp
jag::enforce(ptr);          // throws if ptr is null/false/zero
jag::enforce(count > 0);    // throws if expression is false
```

### Custom validator

A callable that takes the value (or nothing) and returns `bool`:

```cpp
jag::enforce(config.port, [](int p) -> bool { return p > 0 && p < 65536; });

// nullary validator (captures state)
bool ready = check_system();
jag::enforce(0, [&] { return ready; });
```

### Custom error message

A callable that returns something convertible to `std::string`. It can optionally take the enforced value:

```cpp
// nullary: string literal (const char* is convertible to std::string)
jag::enforce(false, [] { return "something went wrong"; });

// nullary: explicit std::string
jag::enforce(false, [] { return std::string("something went wrong"); });

// with value: include it in the message
jag::enforce(false, [](auto&& value) {
    return std::format("Not allowed: {}", value);
});
```

### Custom raiser

A callable that takes `std::string` and returns `void` (typically throws):

```cpp
jag::enforce(false, [](std::string msg) {
    throw std::invalid_argument(msg);
});
```

If no raiser is provided, `enforce` throws `std::runtime_error`.

### Combining callables

Validators, stringables, and raisers can be mixed freely. Each callable is dispatched by its signature via C++20 concepts:

```cpp
jag::enforce(response.status,
    [](int code) -> bool { return code >= 200 && code < 300; },  // validator
    [](int code) { return std::format("HTTP {}", code); },       // error message
    [](std::string msg) { throw HttpError(msg); }                // custom raiser
);
```

### Multiple validators

Validators are folded with `&&` and short-circuit:

```cpp
jag::enforce(value,
    [](auto&& v) -> bool { return v > 0; },     // must be positive
    [](auto&& v) -> bool { return v % 2 == 0; }  // must be even
);
```

### The ENFORCE macro

The `ENFORCE` macro stringifies the expression and supports `std::format` arguments:

```cpp
ENFORCE(x > 0);
// throws: ":Expression 'x > 0' failed"

ENFORCE(x > 0, "expected positive, got {}", x);
// throws: "expected positive, got -3:Expression 'x > 0' failed"
```

## Callable dispatch

Only the callables after the first argument are dispatched. `enforce` inspects each callable's signature at compile time using concepts:

| Concept | Signature | Role |
|---|---|---|
| `validator` | `(T) -> bool` or `() -> bool` | Determines pass/fail |
| `stringable` | `(T) -> string` or `() -> string` | Returns error text (any type convertible to `std::string`, including `const char*`) |
| `raiser` | `(string) -> void` | Throws custom exception |

A callable that matches none of these concepts is ignored.

**Defaults** when a role is absent:
- No validator: the value itself is tested via `static_cast<bool>`.
- No stringable: the default message is `"Expression has failed"`.
- No raiser: `std::runtime_error` is thrown.

## Execution flow

1. **Validate** -- each `validator` callable is invoked. If any returns `false`, enforcement fails. Falls back to bool conversion if none exist.
2. **Build message** -- each `stringable` callable contributes to the error string.
3. **Raise** -- each `raiser` callable receives the message. If none exist, throws `std::runtime_error`.
4. **Pass-through** -- on success, returns the original value with perfect forwarding.

## Building

```bash
cmake -B build
cmake --build build
```

### Running tests

```bash
cmake -B build -DENABLE_TEST=ON
cmake --build build
./build/Debug/enforce_test     # Windows
./build/enforce_test            # Linux/macOS
```

Tests use [Google Test](https://github.com/google/googletest), fetched automatically via CMake's `FetchContent`.

## License

See LICENSE file for details.
