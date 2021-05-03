project(loess)

# LOESS regression implementation, taken from here: https://github.com/bartverm/loess
set(LOESS_SOURCES
	loess.cpp
	loess.h
	Point.hpp)

# If not already set up, install CGAL and Eigen using Conan
if(NOT DEFINED CONAN_CGAL_ROOT)
	include(cmake/conan.cmake)
	conan_check(VERSION 1.0.0 REQUIRED)
	message(STATUS "Downloading dependency libraries")
	conan_cmake_run(REQUIRES cgal/[>=5.1] eigen/[>=3.3.7]
		BUILD missing
		CMAKE_TARGETS
		BASIC_SETUP)
endif()

add_library(loess STATIC ${LOESS_SOURCES})
target_include_directories(loess PUBLIC .)
target_link_libraries(loess CONAN_PKG::eigen CONAN_PKG::cgal)