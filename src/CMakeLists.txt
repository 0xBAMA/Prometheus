# CMakeList.txt : CMake project for Prometheus, include source and define
# project specific logic here.

# Add source to this project's executable.
add_executable ( Prometheus
	Prometheus.cpp
	Prometheus.h
	engine.cpp
	engine.h
)

add_subdirectory( third_party )

set_property( TARGET Prometheus PROPERTY CXX_STANDARD 20 )
set_property( TARGET Prometheus PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:Prometheus> )

target_include_directories( Prometheus PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" )
target_link_libraries( Prometheus vkguide_shared vkbootstrap imgui )

add_custom_command(TARGET Prometheus POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:Prometheus> $<TARGET_FILE_DIR:Prometheus>
	COMMAND_EXPAND_LISTS
)
