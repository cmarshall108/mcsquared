add_library(mc_inventory inventory.cpp)
target_link_libraries(mc_inventory
	PUBLIC mc_item mc_public_headers
	PRIVATE mc_project_options
)