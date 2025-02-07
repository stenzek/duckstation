# Use C++20.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# C++20 feature checks. Some Linux environments are incomplete.
check_cpp20_feature("__cpp_structured_bindings" 201606)
check_cpp20_feature("__cpp_constinit" 201907)
check_cpp20_feature("__cpp_designated_initializers" 201707)
check_cpp20_feature("__cpp_using_enum" 201907)
check_cpp20_feature("__cpp_lib_bit_cast" 201806)
check_cpp20_feature("__cpp_lib_bitops" 201907)
check_cpp20_feature("__cpp_lib_int_pow2" 202002)
check_cpp20_feature("__cpp_lib_starts_ends_with" 201711)
check_cpp20_attribute("likely" 201803)
check_cpp20_attribute("unlikely" 201803)
check_cpp20_attribute("no_unique_address" 201803)
