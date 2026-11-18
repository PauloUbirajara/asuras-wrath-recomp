# xxx_unpack: decompress .xxx packages to plain UE3 bodies (asset extraction).
add_executable(xxx_unpack
    main.cpp
    "${CMAKE_SOURCE_DIR}/tools/mantra_shadergen/xxx_package.cpp")
target_include_directories(xxx_unpack PRIVATE "${CMAKE_SOURCE_DIR}/tools/mantra_shadergen")
target_link_libraries(xxx_unpack PRIVATE mspack)
set_target_properties(xxx_unpack PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
