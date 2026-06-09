# CMakeList.txt : CMake project for Prometheus, include source and define
# project specific logic here.

add_subdirectory( third_party )

# Add source to this project's executable.
add_executable ( Prometheus
		Prometheus.cpp
		Prometheus.h
		engine.cpp
		engine.h
		third_party/volk/volk.h
		third_party/volk/volk.c
)

set_property( TARGET Prometheus PROPERTY CXX_STANDARD 20 )
set_property( TARGET Prometheus PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:Prometheus> )

target_include_directories( Prometheus PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" )
target_include_directories( Prometheus PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fastgltf/include/" )
target_include_directories( Prometheus PUBLIC ${SDL3_INCLUDE_DIRS})

target_link_libraries( Prometheus vkguide_shared vkbootstrap imgui yaml-cpp STB_ImageUtilsWrapper JakobReflectance )

add_custom_command(TARGET Prometheus POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:Prometheus> $<TARGET_FILE_DIR:Prometheus>
	COMMAND_EXPAND_LISTS
)