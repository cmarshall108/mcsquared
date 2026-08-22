add_executable(protocol_tests protocol_tests.cpp)
target_link_libraries(protocol_tests PRIVATE mc_protocol mc_project_options)
target_compile_options(protocol_tests PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-UNDEBUG>
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
)
add_test(NAME protocol_tests COMMAND protocol_tests)

add_executable(world_tests world_tests.cpp)
target_link_libraries(world_tests PRIVATE mc_world mc_project_options)
target_compile_options(world_tests PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-UNDEBUG>
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
)
add_test(NAME world_tests COMMAND world_tests)

add_executable(content_tests content_tests.cpp)
target_link_libraries(content_tests PRIVATE mc_block mc_inventory mc_project_options)
target_compile_options(content_tests PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-UNDEBUG>
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
)
target_compile_definitions(content_tests PRIVATE
    MC_RUNTIME_RECIPES_PATH="${PROJECT_SOURCE_DIR}/docs/generated/recipes-26.2.mcr"
    MC_RUNTIME_REGISTRIES_PATH="${PROJECT_SOURCE_DIR}/docs/generated/registries-26.2.mcr"
)
add_test(NAME content_tests COMMAND content_tests)

add_executable(entity_tests entity_tests.cpp)
target_link_libraries(entity_tests PRIVATE mc_entity mc_project_options)
target_compile_options(entity_tests PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-UNDEBUG>
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
)
target_compile_definitions(entity_tests PRIVATE
    MC_RUNTIME_REGISTRIES_PATH="${PROJECT_SOURCE_DIR}/docs/generated/registries-26.2.mcr"
)
add_test(NAME entity_tests COMMAND entity_tests)

add_executable(player_tests player_tests.cpp)
target_link_libraries(player_tests PRIVATE mc_player mc_project_options)
target_compile_options(player_tests PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-UNDEBUG>
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
)
add_test(NAME player_tests COMMAND player_tests)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME status_integration_test
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/status_integration_test.py
            $<TARGET_FILE:mcsquared>
)
set_tests_properties(status_integration_test PROPERTIES TIMEOUT 15)

add_test(
    NAME session_capture_test
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/session_capture_test.py
            ${PROJECT_SOURCE_DIR}/tools/session-capture/proxy.py
)
set_tests_properties(session_capture_test PROPERTIES TIMEOUT 15)

add_test(
    NAME official_session_conformance_test
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/official_session_conformance_test.py
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/official-status-26.2.ndjson
)
set_tests_properties(official_session_conformance_test PROPERTIES TIMEOUT 15)

add_test(
    NAME login_integration_test
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/login_integration_test.py
            $<TARGET_FILE:mcsquared>
)
set_tests_properties(login_integration_test PROPERTIES TIMEOUT 15)

add_test(
    NAME hardcore_login_integration_test
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/login_integration_test.py
            $<TARGET_FILE:mcsquared>
            --hardcore
)
set_tests_properties(hardcore_login_integration_test PROPERTIES TIMEOUT 15)

add_test(
    NAME registry_fallback_integration_test
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/login_integration_test.py
            $<TARGET_FILE:mcsquared>
            --reject-known-pack
            --fallback-manifest
            ${PROJECT_SOURCE_DIR}/docs/generated/registry-fallback-26.2.mcr
)
set_tests_properties(registry_fallback_integration_test PROPERTIES TIMEOUT 15)

add_test(
    NAME resource_pack_integration_test
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/login_integration_test.py
            $<TARGET_FILE:mcsquared>
            --required-resource-pack
)
set_tests_properties(resource_pack_integration_test PROPERTIES TIMEOUT 15)

add_test(
    NAME play_pipeline_transition_test
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/login_integration_test.py
            $<TARGET_FILE:mcsquared>
            --verify-delayed-keepalive
)
set_tests_properties(play_pipeline_transition_test PROPERTIES TIMEOUT 15)

add_test(
    NAME transfer_integration_test
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/login_integration_test.py
            $<TARGET_FILE:mcsquared>
            --transfer
)
set_tests_properties(transfer_integration_test PROPERTIES TIMEOUT 15)

add_test(
    NAME connection_limits_test
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/connection_limits_test.py
            $<TARGET_FILE:mcsquared>
)
set_tests_properties(connection_limits_test PROPERTIES TIMEOUT 15)

add_test(
    NAME connection_rate_limit_test
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/connection_rate_limit_test.py
            $<TARGET_FILE:mcsquared>
)
set_tests_properties(connection_rate_limit_test PROPERTIES TIMEOUT 15)

add_test(
    NAME login_disconnect_test
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/login_disconnect_test.py
            $<TARGET_FILE:mcsquared>
)
set_tests_properties(login_disconnect_test PROPERTIES TIMEOUT 15)

add_test(
    NAME duplicate_login_test
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/duplicate_login_test.py
            $<TARGET_FILE:mcsquared>
)
set_tests_properties(duplicate_login_test PROPERTIES TIMEOUT 15)

execute_process(
    COMMAND ${Python3_EXECUTABLE} -c "import cryptography"
    RESULT_VARIABLE MC_HAS_PYTHON_CRYPTOGRAPHY
    OUTPUT_QUIET
    ERROR_QUIET
)
if(MC_HAS_PYTHON_CRYPTOGRAPHY EQUAL 0)
    add_test(
        NAME encrypted_login_integration_test
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/login_integration_test.py
                $<TARGET_FILE:mcsquared>
                --encrypted-offline
    )
    set_tests_properties(encrypted_login_integration_test PROPERTIES TIMEOUT 15)

    add_test(
        NAME online_login_integration_test
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/login_integration_test.py
                $<TARGET_FILE:mcsquared>
                --online-mode
                --custom-query
    )
    set_tests_properties(online_login_integration_test PROPERTIES TIMEOUT 15)
endif()

add_test(
    NAME protocol_manifest_test
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/protocol_manifest_test.py
            ${PROJECT_SOURCE_DIR}/tools/mc-extract/extract.py
            ${PROJECT_SOURCE_DIR}/tools/mc-extract/server.jar
)
set_tests_properties(protocol_manifest_test PROPERTIES TIMEOUT 30)

add_test(
    NAME content_manifest_test
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/content_manifest_test.py
            ${PROJECT_SOURCE_DIR}/tools/mc-extract/extract.py
            ${PROJECT_SOURCE_DIR}/tools/mc-extract/server.jar
)
set_tests_properties(content_manifest_test PROPERTIES TIMEOUT 30)