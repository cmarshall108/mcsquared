add_library(mc_entity
	ai.cpp
	animal.cpp
	dropped_item.cpp
	entity.cpp
	projectile.cpp
	spawning.cpp
	tracking.cpp
)
target_link_libraries(mc_entity
	PUBLIC mc_world mc_item mc_core mc_public_headers
	PRIVATE mc_project_options
)