find_package(Qt6 6.11 REQUIRED COMPONENTS Core)

add_executable(cli_contract_test cli_contract_test.cpp)
target_link_libraries(cli_contract_test PRIVATE Qt6::Core)
target_compile_features(cli_contract_test PRIVATE cxx_std_20)
add_dependencies(cli_contract_test rmk)
add_test(NAME cli_contract COMMAND cli_contract_test $<TARGET_FILE:rmk>)
# Fixture commits must not wait on the user's signing key (a stale gpg lock
# once hung them); git's env config overrides the global setting.
set_tests_properties(cli_contract PROPERTIES
    ENVIRONMENT "GIT_CONFIG_COUNT=1;GIT_CONFIG_KEY_0=commit.gpgsign;GIT_CONFIG_VALUE_0=false")

add_test(NAME cli_no_gui COMMAND ${CMAKE_COMMAND}
    -DBINARY=$<TARGET_FILE:rmk>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/no_gui_dependency.cmake)
