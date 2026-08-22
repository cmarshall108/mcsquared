add_subdirectory(core)
add_subdirectory(network)
add_subdirectory(world)
add_subdirectory(protocol)
add_subdirectory(block)
add_subdirectory(item)
add_subdirectory(inventory)
add_subdirectory(entity)
add_subdirectory(player)
add_subdirectory(gameplay)
add_subdirectory(dimension)
add_subdirectory(structure)
add_subdirectory(mod)
add_subdirectory(server)

add_executable(mcsquared main.cpp)
target_link_libraries(mcsquared PRIVATE mcsquared_server_runtime mc_project_options)
set_target_properties(mcsquared PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
	RUNTIME_OUTPUT_DIRECTORY_DEBUG "${PROJECT_BINARY_DIR}"
	RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PROJECT_BINARY_DIR}"
	RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${PROJECT_BINARY_DIR}"
	RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${PROJECT_BINARY_DIR}"
)