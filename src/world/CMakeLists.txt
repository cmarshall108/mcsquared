add_library(mc_world
	chunk.cpp
	generation.cpp
	storage.cpp
	ticks.cpp
	world.cpp
)
target_link_libraries(mc_world
	PUBLIC mc_core mc_public_headers
	PRIVATE mc_project_options ZLIB::ZLIB
)