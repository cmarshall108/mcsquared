add_library(mcsquared_server_runtime
    server.cpp
    session_auth.cpp
)
target_link_libraries(mcsquared_server_runtime
    PUBLIC mc_protocol mc_world mc_public_headers
    PRIVATE mc_block mc_entity mc_player CURL::libcurl Threads::Threads mc_project_options
            $<$<PLATFORM_ID:Windows>:ws2_32>
)
target_compile_definitions(mcsquared_server_runtime PRIVATE
    MC_RUNTIME_TAGS_PATH="${PROJECT_SOURCE_DIR}/docs/generated/tags-26.2.mcr"
    MC_RUNTIME_NETWORK_REGISTRIES_PATH="${PROJECT_SOURCE_DIR}/docs/generated/network-registries-26.2.mcr"
    MC_RUNTIME_REGISTRY_FALLBACK_PATH="${PROJECT_SOURCE_DIR}/docs/generated/registry-fallback-26.2.mcr"
    MC_RUNTIME_RECIPE_SYNC_PATH="${PROJECT_SOURCE_DIR}/docs/generated/recipe-sync-26.2.mcr"
    MC_RUNTIME_REGISTRIES_PATH="${PROJECT_SOURCE_DIR}/docs/generated/registries-26.2.mcr"
)