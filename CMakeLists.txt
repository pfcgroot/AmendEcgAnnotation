# CMakeList.txt : CMake project for AmendEcgAnnotation, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.8)
#set(DCMTK_DIR "D:\\Projects\\dcmtk-3.6.6")

find_package(DCMTK NO_MODULE)
include_directories(${DCMTK_INCLUDE_DIR})

 
project ("AmendEcgAnnotation")

# Add source to this project's executable.
add_executable (AmendEcgAnnotation "AmendEcgAnnotation.cpp" "AmendEcgAnnotation.h")

target_link_libraries(AmendEcgAnnotation ${DCMTK_LIBRARIES}) # also adds the required include path

