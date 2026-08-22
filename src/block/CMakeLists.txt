add_library(mc_block block.cpp)
target_link_libraries(mc_block
	PUBLIC mc_core mc_item mc_world mc_public_headers
	PRIVATE mc_project_options
)