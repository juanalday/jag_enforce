CMAKE_MINIMUM_REQUIRED(VERSION 3.14 FATAL_ERROR)
PROJECT(enforce CXX)

option(ENABLE_TEST "Enable tests" ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(enforce INTERFACE enforce.hpp)
if(WIN32)
target_compile_options(enforce INTERFACE /Zc:preprocessor /Zc:__cplusplus)
endif()

if(ENABLE_TEST)
  enable_testing()
  include(FetchContent)
  FetchContent_Declare(googletest
     GIT_REPOSITORY https://github.com/google/googletest.git
	 GIT_TAG main
  )
  # For Windows: Prevent overriding the parent project's compiler/linker settings
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  add_executable(enforce_test
        enforce.hpp
	    enforce.t.cpp
  )
	target_link_libraries(enforce_test enforce gtest gtest_main)

  include(GoogleTest)
  gtest_discover_tests(enforce_test)

endif()
