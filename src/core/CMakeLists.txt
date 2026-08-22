add_library(mc_core types.cpp)
target_link_libraries(mc_core
	PUBLIC mc_public_headers
	PRIVATE mc_project_options
)