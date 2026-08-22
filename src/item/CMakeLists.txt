add_library(mc_item item.cpp)
target_link_libraries(mc_item
	PUBLIC mc_core mc_public_headers
	PRIVATE mc_project_options
)