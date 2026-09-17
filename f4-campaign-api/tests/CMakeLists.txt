# f4-campaign-api tests — the contract pinned with no engine present.
#
# GoogleTest is fetched and the GoogleTest module included from the root
# CMakeLists.txt (via cmake/f4_deps.cmake). This file only registers test
# executables via gtest_discover_tests(), matching every f4-* tests dir.

function(f4_campaign_api_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-campaign-api)
    gtest_discover_tests(${name})
endfunction()

# Command wire: parse → re-encode round trips, canonical bytes, the
# malformed-rejects-loudly discipline.
f4_campaign_api_add_test(test_command_wire test_command_wire.cpp)

# DTO encoders: byte-stable golden strings (the canonical key order IS
# the contract) + the event vocabulary pinned before HOST-2 emits it.
f4_campaign_api_add_test(test_dto_goldens test_dto_goldens.cpp)

# The protocol dispatcher against a MOCK session: op routing, the strict
# envelope, exit-code mapping, refusal-as-data. No engine in the link.
f4_campaign_api_add_test(test_protocol_dispatch test_protocol_dispatch.cpp)

# HOST-2: the journal's byte discipline (one golden line per event
# family, replay identity, drift naming) + the subscription filter's
# per-family team semantics. Still no engine in the link.
f4_campaign_api_add_test(test_event_journal test_event_journal.cpp)
