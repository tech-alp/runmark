# Version handling, before project(). semantic-release injects RUNMARK_VERSION
# at release time; a local build falls back to the value committed here.
include_guard(GLOBAL)

set(RUNMARK_VERSION "0.4.0" CACHE STRING "Release version injected by semantic-release")
if(NOT RUNMARK_VERSION MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
    message(FATAL_ERROR "RUNMARK_VERSION must be a stable x.y.z version, got '${RUNMARK_VERSION}'")
endif()
