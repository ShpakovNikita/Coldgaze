cmake_minimum_required(VERSION 2.8 FATAL_ERROR)
cmake_policy(VERSION 2.8)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake")
set(CMAKE_BINARY_DIR "${CMAKE_SOURCE_DIR}/bin")
set(COLDGAZE_SOURCE_PATH ${COLDGAZE_SOURCE_PATH} "${CMAKE_SOURCE_DIR}/src")

if(WIN32)
    set(COLDGAZE_LIBS_PATH ${CMAKE_SOURCE_DIR}/libs/win32)
else(WIN32)
    message("Non win32 systems not supported from the box, some errors may be occured!")
endif()

set(NAME Coldgaze)

project(${NAME})

# Use FindVulkan module added with CMAKE 3.7
if(NOT CMAKE_VERSION VERSION_LESS 3.7.0)
	message(STATUS "Using module to find Vulkan")
	find_package(Vulkan)
endif()

if(WIN32)
	if (NOT Vulkan_FOUND)
		find_library(Vulkan_LIBRARY NAMES vulkan-1 vulkan PATHS ${COLDGAZE_LIBS_PATH}/vulkan)
		if (Vulkan_LIBRARY)
			set(Vulkan_FOUND ON)
			MESSAGE("Using bundled Vulkan library version")
		endif()
	endif()
	find_library(ASSIMP_LIBRARIES NAMES assimp libassimp.dll.a PATHS ${COLDGAZE_LIBS_PATH}/assimp)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DVK_USE_PLATFORM_WIN32_KHR")
else(WIN32)
	if (NOT Vulkan_FOUND)
		find_library(Vulkan_LIBRARY NAMES vulkan HINTS "$ENV{VULKAN_SDK}/lib" "${COLDGAZE_LIBS_PATH}/vulkan" REQUIRED)
		if (Vulkan_LIBRARY)
			set(Vulkan_FOUND ON)
			MESSAGE("Using bundled Vulkan library version")
		endif()
	endif()
	find_package(ASSIMP REQUIRED)
	find_package(Threads REQUIRED)
endif(WIN32)

if (NOT Vulkan_FOUND)
	message(FATAL_ERROR "Could not find Vulkan library!")
else()
	message(STATUS ${Vulkan_LIBRARY})
endif()

find_package(SDL2 REQUIRED)

if(WIN32)
	if (NOT SDL2_FOUND)
		find_library(SDL2_LIB NAMES sdl2 PATHS ${COLDGAZE_LIBS_PATH}/SDL2)
        find_library(SDL2_LIB_MAIN NAMES sdl2main PATHS ${COLDGAZE_LIBS_PATH}/SDL2)
        set(SDL2_LIBRARY ${SDL2_LIB} ${SDL2_LIB_MAIN})
        
		if (SDL2_LIBRARY)
			set(SDL2_FOUND ON)
			MESSAGE("Using bundled Sdl2 library version")
		endif()
	endif()
endif(WIN32)

if (NOT SDL2_FOUND)
	message(FATAL_ERROR "Could not find Sdl2 library!")
else()
	message(STATUS ${SDL2_LIBRARY})
endif()

include_directories("include")
include_directories("include/glm")
include_directories("include/entt/src")

# Set preprocessor defines
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DNOMINMAX -D_USE_MATH_DEFINES")

# Clang specific stuff
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-switch-enum")
endif()

add_definitions(-D_CRT_SECURE_NO_WARNINGS)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_definitions(-std=c++17)
endif()

if(RESOURCE_INSTALL_DIR)
	add_definitions(-DVK_EXAMPLE_DATA_DIR=\"${RESOURCE_INSTALL_DIR}/\")
	install(DIRECTORY data/ DESTINATION ${RESOURCE_INSTALL_DIR}/)
else()
	add_definitions(-DVK_EXAMPLE_DATA_DIR=\"${CMAKE_SOURCE_DIR}/data/\")
endif()

# Compiler specific stuff
if(MSVC)
	SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc")
endif(MSVC)

if(WIN32)
	# Nothing here (yet)
else(WIN32)
	link_libraries(${Vulkan_LIBRARY} ${ASSIMP_LIBRARIES})
endif(WIN32)

add_subdirectory(src)