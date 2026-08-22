add_library(mc_protocol
	codec.cpp
	crypto.cpp
	nbt.cpp
	packets.cpp
)
target_link_libraries(mc_protocol
	PUBLIC mc_public_headers mc_world
	PRIVATE mc_project_options OpenSSL::Crypto ZLIB::ZLIB
)