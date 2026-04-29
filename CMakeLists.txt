cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

# Set the project name
project(testRunner)

# Enable testing
enable_testing()

# Set the C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_EXTENSIONS False)
  
# Add sub directories
add_subdirectory(Moira)
add_subdirectory(Runner/Binutils)
add_subdirectory(Runner/Musashi)

# Add the executable
add_executable(testRunner
Runner/main.cpp
Runner/Testrunner.cpp
Runner/TestCPU.cpp
Runner/musashi.cpp
Runner/Sandbox.cpp
Runner/Randomizer.cpp
)
target_link_libraries(testRunner binutils)
target_link_libraries(testRunner moira)
target_link_libraries(testRunner musashi)

# Add compile options
if(MSVC)
  target_compile_options(testRunner PUBLIC /wd4100 /wd4201 /wd4324 /wd4458)
else()
  target_compile_options(testRunner PUBLIC -Wno-unused-parameter)
  target_compile_options(testRunner PUBLIC -Wno-unused-variable)
  target_compile_options(testRunner PUBLIC -Wno-gnu-anonymous-struct)
  target_compile_options(testRunner PUBLIC -Wno-nested-anon-types)
endif()

# Add include paths
target_include_directories(testRunner PUBLIC

${CMAKE_SOURCE_DIR}/.
${CMAKE_SOURCE_DIR}/Moira
${CMAKE_SOURCE_DIR}/Runner
${CMAKE_SOURCE_DIR}/Runner/Binutils
${CMAKE_SOURCE_DIR}/Runner/Musashi
)

# Add tests
add_test(UnitTest testRunner)

# FPU and 040/060 test suite
add_executable(fpuTests Tests/Test_FPU_GoldenVectors.cpp)
target_link_libraries(fpuTests moira)
target_include_directories(fpuTests PRIVATE ${CMAKE_SOURCE_DIR}/Moira ${CMAKE_SOURCE_DIR}/Moira/softfloat)
if(NOT MSVC)
  target_compile_options(fpuTests PRIVATE -Wno-unused-parameter)
endif()

add_executable(comprehensiveTests Tests/Test_FPU_040_Functional.cpp)
target_link_libraries(comprehensiveTests moira)
target_include_directories(comprehensiveTests PRIVATE ${CMAKE_SOURCE_DIR}/Moira ${CMAKE_SOURCE_DIR}/Moira/softfloat)
if(NOT MSVC)
  target_compile_options(comprehensiveTests PRIVATE -Wno-unused-parameter)
endif()

add_executable(edgeCaseTests Tests/Test_FPU_040_EdgeCases.cpp)
target_link_libraries(edgeCaseTests moira)
target_include_directories(edgeCaseTests PRIVATE ${CMAKE_SOURCE_DIR}/Moira ${CMAKE_SOURCE_DIR}/Moira/softfloat)
if(NOT MSVC)
  target_compile_options(edgeCaseTests PRIVATE -Wno-unused-parameter)
endif()

add_executable(advancedTests Tests/Test_FPU_040_Stress.cpp)
target_link_libraries(advancedTests moira)
target_include_directories(advancedTests PRIVATE ${CMAKE_SOURCE_DIR}/Moira ${CMAKE_SOURCE_DIR}/Moira/softfloat)
if(NOT MSVC)
  target_compile_options(advancedTests PRIVATE -Wno-unused-parameter)
endif()

add_test(NAME FPUTests COMMAND fpuTests)
add_test(NAME ComprehensiveTests COMMAND comprehensiveTests)
add_test(NAME EdgeCaseTests COMMAND edgeCaseTests)
add_test(NAME AdvancedTests COMMAND advancedTests)
