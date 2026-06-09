find_package(Vulkan REQUIRED)

add_library(vkbootstrap STATIC)
add_library(glm INTERFACE)
add_library(vma INTERFACE)

add_library(stb_image INTERFACE)

add_subdirectory( yaml-cpp )

add_subdirectory(fastgltf)
add_subdirectory(fmt EXCLUDE_FROM_ALL)

# Include the command that downloads libraries
include(FetchContent)

# define a function for adding git dependencies
function(include_dependency libName gitURL gitTag)
	# setup the declare
	FetchContent_Declare(${libName}
			GIT_REPOSITORY ${gitURL}
			GIT_TAG        ${gitTag}
			GIT_SHALLOW    TRUE
			GIT_PROGRESS   TRUE
	)

	FetchContent_MakeAvailable(${libName})
endfunction()

# add SDL3 support
find_package(SDL3 QUIET)
if (NOT SDL3_FOUND)
	message(STATUS "Getting SDL3 from Github")
	include_dependency(SDL3 https://github.com/libsdl-org/SDL.git release-3.4.2)
else()
	message(STATUS "Using local SDL3")
endif()

target_sources(vkbootstrap PRIVATE
	vkbootstrap/VkBootstrap.h
	vkbootstrap/VkBootstrap.cpp
)

target_include_directories(vkbootstrap PUBLIC vkbootstrap)
target_link_libraries(vkbootstrap PUBLIC Vulkan::Vulkan $<$<BOOL:UNIX>:${CMAKE_DL_LIBS}>)
set_property(TARGET vkbootstrap PROPERTY CXX_STANDARD 20)

#both vma and glm and header only libs so we only need the include path
target_include_directories(vma INTERFACE vma)
target_include_directories(glm INTERFACE glm)

add_library( STB_ImageUtilsWrapper STATIC stb/impl.cc )

add_library(imgui STATIC)

target_include_directories(imgui PUBLIC imgui)

target_sources(imgui PRIVATE 
  imgui/imgui.h
  imgui/imgui.cpp

  imgui/imgui_demo.cpp
  imgui/imgui_draw.cpp
  imgui/imgui_widgets.cpp
  imgui/imgui_tables.cpp
  imgui/imgui_impl_vulkan.cpp
  imgui/imgui_impl_sdl3.cpp
)

add_library( JakobReflectance STATIC
	Jakob2019Spectral/supplement/rgb2spec.cc
)

target_link_libraries( imgui PUBLIC Vulkan::Vulkan SDL3::SDL3 )

target_include_directories(stb_image INTERFACE stb_image)
