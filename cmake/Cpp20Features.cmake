# Probes the C++20 library features that differ between MSVC 2019 and MSVC 2022.
# Results are exposed as an INTERFACE target so every module inherits the same
# HWSIM_HAS_* definitions; call sites guard on those instead of compiler version
# checks scattered through the source tree.

include(CheckCXXSourceCompiles)

set(CMAKE_REQUIRED_FLAGS "")
if(MSVC)
    set(CMAKE_REQUIRED_FLAGS "/std:c++20 /Zc:__cplusplus")
else()
    set(CMAKE_REQUIRED_FLAGS "-std=c++20")
endif()

check_cxx_source_compiles("
#include <format>
#include <string>
int main() { std::string s = std::format(\"{}-{}\", 1, 2); return s.empty() ? 1 : 0; }
" HWSIM_HAS_STD_FORMAT)

check_cxx_source_compiles("
#include <ranges>
#include <vector>
int main() {
    std::vector<int> v{1,2,3};
    auto r = v | std::views::filter([](int i){ return i > 1; });
    int n = 0; for (int i : r) n += i; return n == 5 ? 0 : 1;
}
" HWSIM_HAS_RANGES)

check_cxx_source_compiles("
#include <source_location>
int main() { return std::source_location::current().line() > 0 ? 0 : 1; }
" HWSIM_HAS_SOURCE_LOCATION)

check_cxx_source_compiles("
#include <bit>
#include <cstdint>
int main() { return std::byteswap(std::uint32_t{1}) != 0 ? 0 : 1; }
" HWSIM_HAS_STD_BYTESWAP)

unset(CMAKE_REQUIRED_FLAGS)

add_library(hwsim_cpp20 INTERFACE)
add_library(hwsim::cpp20 ALIAS hwsim_cpp20)
target_compile_features(hwsim_cpp20 INTERFACE cxx_std_20)

foreach(_feature IN ITEMS
        HWSIM_HAS_STD_FORMAT
        HWSIM_HAS_RANGES
        HWSIM_HAS_SOURCE_LOCATION
        HWSIM_HAS_STD_BYTESWAP)
    if(${_feature})
        target_compile_definitions(hwsim_cpp20 INTERFACE ${_feature}=1)
    else()
        target_compile_definitions(hwsim_cpp20 INTERFACE ${_feature}=0)
    endif()
endforeach()
