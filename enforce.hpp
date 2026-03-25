#ifndef JAG_ENFORCE_HPP
#define JAG_ENFORCE_HPP
#include <concepts>
#include <format>
#include <functional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#define ENFORCE_PP_CAT(a, b) ENFORCE_PP_CAT_I(a, b)
#define ENFORCE_PP_CAT_I(a, b) a ## b
#define ENFORCE_PP_STRINGIZE(text) ENFORCE_PP_STRINGIZE_I(text)
#define ENFORCE_PP_STRINGIZE_I(...) #__VA_ARGS__

#define ENFORCE(exp, ...) \
  jag::enforce(exp \
    __VA_OPT__(, [&]() -> std::string { return std::format(__VA_ARGS__); }) \
    , []() -> std::string { return ":Expression '" ENFORCE_PP_STRINGIZE(exp) "' failed"; })

namespace jag {
	namespace detail {

		// Matches types that are exactly bool (not just convertible to bool)
		template <class T>
		concept same_as_bool_impl = std::same_as<T, bool>;
		template <class T>
		concept same_as_bool = same_as_bool_impl<T> && requires(T && t) {
			{ !static_cast<T&&>(t) } -> same_as_bool_impl;
		};

		// A callable that returns bool, used to override the default truthiness check
		template <class F, class... Args>
		concept validator = std::invocable<F, Args...> && same_as_bool<std::invoke_result_t<F, Args...>>;
		// A callable that returns something convertible to std::string, used to build error messages
		template<class F, class... Args>
		concept stringable = std::invocable<F, Args...> && std::convertible_to<std::invoke_result_t<F, Args...>, std::string>;
		// A callable that takes the error message and throws a custom exception
		template<class F>
		concept raiser = std::invocable<F, std::string> && std::same_as<std::invoke_result_t<F, std::string>, void>;

		// A nullary callable that produces the value to enforce (first argument only)
		template<class T>
		concept value_producer = std::invocable<T> && !std::same_as<std::invoke_result_t<T>, void>;

		// True if any of the callables in Fs... is a validator
		template<typename T, typename... Fs>
		concept any_validator = (... || (validator<Fs, T> || validator<Fs>));

		template<typename T, typename F>
		bool validate_impl(T&& t, F&& f)
		{
			if constexpr (validator<F>)
			{
				return f();
			}
			else if constexpr (validator<F, T>)
			{
				return f(std::forward<T>(t));
			}
			return true;
		}

		template<typename T, typename... Args>
		bool validate(T&& t, Args&&... args) {
			if constexpr (any_validator<T, Args...>)
			{
				return (... && validate_impl(std::forward<T>(t), std::forward<Args>(args)));
			}

			if constexpr (std::convertible_to<T, bool>)
				return static_cast<bool>(t);
			return false;
		}

		template<typename T, typename... Args>
		constexpr bool wrong(T&& t, Args&&... args) {
			return !validate(std::forward<T>(t), std::forward<Args>(args)...);
		}

		template<typename T, typename F>
		constexpr void append_impl(std::string& buffer, T&& t, F&& f) {
			if constexpr (stringable<F>)
			{
				buffer += std::string(f());
			}
			else if constexpr (stringable<F, T>)
			{
				buffer += std::string(f(std::forward<T>(t)));
			}
		}

		template<typename T, typename... Args>
		std::string append(T&& t, Args&&... args) {
			std::string buffer;
			(..., append_impl(buffer, std::forward<T>(t), std::forward<Args>(args)));
			if (buffer.empty())
				buffer = "Expression has failed";
			return buffer;
		}

		template<typename F>
		void raise_impl(std::string const& msg, F&& f)
		{
			if constexpr (raiser<F>)
			{
				f(msg);
			}
		}

		template<typename T, typename... Args>
		void raise(std::string const& msg, T&& t, Args&&... args) {
			(raise_impl(msg, std::forward<T>(t)), ..., raise_impl(msg, std::forward<Args>(args)));
			throw std::runtime_error(msg);
		}

	}

	template<typename T, typename... Args>
	decltype(auto) enforce(T&& t, Args&&... args) {
		//	If T is a value producer, call it to get the value to enforce. Otherwise, enforce T itself.
		if constexpr (detail::value_producer<T>) {
			using value_type = std::invoke_result_t<T>;
			value_type value = std::invoke(std::forward<T>(t));
			// If the value is a reference, we want to return it directly so that it can be modified by the caller.
			// If it's not a reference, we can enforce it and then return it by value.
			if constexpr (std::is_reference_v<value_type>) {
				return enforce(std::forward<value_type>(value), std::forward<Args>(args)...);
			} else {
				enforce(value, std::forward<Args>(args)...);
				return value;
			}
		} else {
			if (detail::wrong(std::forward<T>(t), std::forward<Args>(args)...)) {
				std::string const msg = detail::append(std::forward<T>(t), std::forward<Args>(args)...);
				detail::raise(msg, std::forward<T>(t), std::forward<Args>(args)...);
			}
			return std::forward<T>(t);
		}
	}
} // end of namespace jag

#endif // JAG_ENFORCE_HPP
