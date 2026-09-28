foreach(name workflow state_exclude base_ref preserved_worktree plan_rules)
    add_executable(${name}_test ${name}_test.cpp)
    target_link_libraries(${name}_test PRIVATE runmark::application Qt6::Core)
    add_test(NAME ${name} COMMAND ${name}_test)
    # Fixture commits must not wait on the user's signing key (a stale gpg
    # lock once hung them); git's env config overrides the global setting.
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "GIT_CONFIG_COUNT=1;GIT_CONFIG_KEY_0=commit.gpgsign;GIT_CONFIG_VALUE_0=false")
endforeach()
