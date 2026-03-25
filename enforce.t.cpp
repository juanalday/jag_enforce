#include "enforce.hpp"
#include <format>
#include <string>
#include <type_traits>
#include <gtest/gtest.h>

struct tester {
    tester()
    {
    }
    tester(tester const&)
    {
		throw std::runtime_error("Copy constructor invoked");
    }
    tester& operator=(tester const&)
    {
		throw std::runtime_error("Assignment operator invoked");
    }
    tester(tester&&)
    {
		throw std::runtime_error("Move copy constructor invoked");
    }
    tester& operator=(tester&&)
    {
		throw std::runtime_error("Move assignment operator invoked");
    }

	static tester make() { return tester(); }
	operator bool() const {return true; }
};

using jag::enforce;
using namespace jag::detail;

TEST(value_producer, basic)
{
	static_assert(value_producer<decltype([] { return 42; }) > );

	EXPECT_TRUE(value_producer<decltype([] { return 42; })>);
	EXPECT_FALSE(value_producer<decltype([] { return void(); })>);
	EXPECT_FALSE(value_producer<int>);

	// testing categories
	{
		int x{ 5 };
		// prvalue: lambda returns int by value → enforce returns int (non-reference)
		EXPECT_TRUE((std::is_same_v<decltype(enforce([&] { return x; })), int > ));
		// lvalue reference: lambda returns int& → enforce returns int& (reference)
		EXPECT_TRUE((std::is_same_v<decltype(enforce([&]() -> int& { return x; })), int& > ));
		// const lvalue reference: lambda returns const int& → enforce returns const int&
		EXPECT_TRUE((std::is_same_v<decltype(enforce([&]() -> const int& { return x; })), const int& > ));
		// xvalue: lambda returns int&& → enforce returns int&&
		EXPECT_TRUE((std::is_same_v<decltype(enforce([&]() -> int&& { return std::move(x); })), int&& > ));
	}
}


TEST(enforce, no_arguments)
{
	EXPECT_NO_THROW(enforce(true));
	EXPECT_THROW(enforce(false), std::runtime_error);
}

TEST(enforce, value_producer)
{
	EXPECT_NO_THROW(enforce([] { return true; }));
	EXPECT_THROW(enforce([] { return false; }), std::runtime_error);

	EXPECT_EQ(enforce([] { return 42; }), 42);

	int* p = nullptr;
	EXPECT_THROW(enforce([&] { return p; }), std::runtime_error);

	int x = 10;
	int* q = &x;
	EXPECT_EQ(enforce([&] { return q; }), &x);
}

TEST(enforce, value_producer_with_callables)
{
	// This test verifies that the value producer overload correctly forwards the callables to the inner enforce call,
	// and that they are invoked with the produced value when appropriate.
	EXPECT_THROW(enforce([] { return false; }, [] { return "produced value was false"; }),std::runtime_error);

	try {
		enforce([] { return false; },
			[] { return "produced value was false"; }
		);
		FAIL() << "Expected std::runtime_error";
	}
	catch (std::runtime_error const& e)
	{
		EXPECT_STREQ(e.what(), "produced value was false");
	}

	EXPECT_NO_THROW(enforce([] { return 5; },
		[](int v) -> bool { return v > 0; }
	));

	EXPECT_THROW(enforce([] { return -1; },
		[](int v) -> bool { return v > 0; }
	), std::runtime_error);
}

TEST(enforce, custom_validator)
{
	using namespace std::literals;
	int const i = 4;
	EXPECT_NO_THROW(enforce(4, [](int v) {return v == 4; }));
	EXPECT_NO_THROW(enforce(4, [](auto v)->bool {return v == 4; }));
	EXPECT_NO_THROW(enforce(4, [](auto const& v)->bool {return v == 4; }));
	EXPECT_NO_THROW(enforce(4, [](auto&& v)->bool {return v == 4; }));
	EXPECT_NO_THROW(enforce(i, [](auto v)->bool {return v == 4; }));
	EXPECT_NO_THROW(enforce(i, [](auto const& v)->bool {return v == 4; }));
	EXPECT_NO_THROW(enforce(i, [](auto&& v)->bool {return v == 4; }));
	EXPECT_NO_THROW(enforce("hello"s, [](auto&& v)->bool {return v == "hello"; }));
	EXPECT_THROW(enforce(100, [](auto v)->bool {return v == 1; }), std::runtime_error);
	EXPECT_THROW(enforce(100, [](auto const& v)->bool {return v == 1; }), std::runtime_error);
	EXPECT_THROW(enforce(100, [](auto&& v)->bool {return v == 1; }), std::runtime_error);
}

TEST(enforce, mutable_validator)
{
	EXPECT_NO_THROW(enforce(1,
		[](auto&& v)->bool {v = 10; return true; }
		, [](auto&& v)->bool {return v == 10; }
	));
	EXPECT_THROW(enforce(1,
		[](auto&& v)->bool {v = 10; return true; }
		, [](auto&& v)->bool {return v == 1; }
	), std::runtime_error);

}

TEST(enforce, custom_error_message)
{
	try {
		enforce(false, [](bool value) {return std::format("Value is {}", value); });
		FAIL() << "Expected std::runtime_error";
	}
	catch (std::runtime_error const& e)
	{
		EXPECT_STREQ(e.what(), "Value is false");
	}
}

TEST(enforce, custom_raiser)
{
	try {
		enforce(false, [](std::string msg) {throw std::runtime_error("Value is false"); });
		FAIL() << "Expected std::runtime_error";
	}
	catch (std::runtime_error const& e)
	{
		EXPECT_STREQ(e.what(), "Value is false");
	}
}
TEST(enforce, perfect_forwarding)
{
	tester value;
	tester const const_value;
	enforce(tester());
	EXPECT_NO_THROW(enforce(tester()));
	EXPECT_NO_THROW(enforce(value));
	EXPECT_NO_THROW(enforce(const_value));
	EXPECT_NO_THROW(tester&& t = enforce(tester()));
	EXPECT_THROW(auto t = enforce(tester()), std::runtime_error);
	EXPECT_THROW(tester t = enforce(tester()), std::runtime_error);
	EXPECT_THROW(tester t = enforce(tester()), std::runtime_error);
}

TEST(enforce, returns_value_passthrough)
{
	EXPECT_EQ(enforce(42), 42);
	EXPECT_EQ(enforce(std::string("hello"), [](auto&&) -> bool { return true; }), "hello");

	int x = 7;
	int& ref = enforce(x);
	ref = 99;
	EXPECT_EQ(x, 99);
}

TEST(enforce, pointer_enforcement)
{
	int value = 10;
	int* p = &value;
	int* null_p = nullptr;

	EXPECT_EQ(enforce(p), &value);
	EXPECT_THROW(enforce(null_p), std::runtime_error);
}

TEST(enforce, default_error_message)
{
	try {
		enforce(false);
		FAIL() << "Expected std::runtime_error";
	}
	catch (std::runtime_error const& e)
	{
		EXPECT_STREQ(e.what(), "Expression has failed");
	}
}

TEST(enforce, nullary_validator)
{
	bool flag = true;
	EXPECT_NO_THROW(enforce(0, [&] { return flag; }));

	flag = false;
	EXPECT_THROW(enforce(0, [&] { return flag; }), std::runtime_error);
}

TEST(enforce, multiple_validators_short_circuit)
{
	int call_count = 0;
	EXPECT_THROW(enforce(true,
		[](auto&&) -> bool { return false; },
		[&](auto&&) -> bool { ++call_count; return true; }
	), std::runtime_error);
	EXPECT_EQ(call_count, 0);
}

TEST(enforce, nullary_string_literal_appender)
{
	try {
		enforce(false, [] { return "literal message"; });
		FAIL() << "Expected std::runtime_error";
	}
	catch (std::runtime_error const& e)
	{
		EXPECT_STREQ(e.what(), "literal message");
	}
}

TEST(enforce, multiple_appenders)
{
	try {
		enforce(false,
			[] { return "first"; },
			[] { return "+second"; }
		);
		FAIL() << "Expected std::runtime_error";
	}
	catch (std::runtime_error const& e)
	{
		EXPECT_STREQ(e.what(), "first+second");
	}
}

TEST(enforce, validator_with_appender)
{
	try {
		enforce(42,
			[](int v) -> bool { return v < 0; },
			[](int v) { return std::format("{} is not negative", v); }
		);
		FAIL() << "Expected std::runtime_error";
	}
	catch (std::runtime_error const& e)
	{
		EXPECT_STREQ(e.what(), "42 is not negative");
	}
}

TEST(enforce, custom_raiser_with_custom_exception)
{
	struct custom_error : std::logic_error {
		using std::logic_error::logic_error;
	};

	EXPECT_THROW(
		enforce(false, [](std::string msg) { throw custom_error(msg); }),
		custom_error
	);
}

TEST(enforce, raiser_receives_appender_message)
{
	std::string captured;
	try {
		enforce(false,
			[] { return std::string("detail: something broke"); },
			[&](std::string msg) { captured = msg; throw std::runtime_error(msg); }
		);
		FAIL() << "Expected std::runtime_error";
	}
	catch (std::runtime_error const&) {}

	EXPECT_EQ(captured, "detail: something broke");
}

TEST(enforce, enforce_macro)
{
	EXPECT_NO_THROW(ENFORCE(true));
	EXPECT_THROW(ENFORCE(false), std::runtime_error);

	try {
		int x = -1;
		ENFORCE(x > 0, "x was {}", x);
		FAIL() << "Expected std::runtime_error";
	}
	catch (std::runtime_error const& e)
	{
		std::string msg = e.what();
		EXPECT_TRUE(msg.find("x was -1") != std::string::npos);
		EXPECT_TRUE(msg.find("x > 0") != std::string::npos);
	}
}
