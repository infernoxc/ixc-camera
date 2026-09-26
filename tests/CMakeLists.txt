add_executable(ixc_unit_tests
    test_main.cpp
    test_json.cpp
    test_profile.cpp
    test_diagnostics.cpp)
target_link_libraries(ixc_unit_tests PRIVATE ixc_core)

add_test(NAME unit COMMAND ixc_unit_tests)
set_tests_properties(unit PROPERTIES TIMEOUT 120)
