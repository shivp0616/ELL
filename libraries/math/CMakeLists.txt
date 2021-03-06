#
# cmake file
#

set(library_name math)

if(MSVC)
  add_compile_options(-D_SCL_SECURE_NO_WARNINGS)
endif()

include (OpenBLASSetup)

set(src src/BlasWrapper.cpp
         src/Tensor.cpp
)

set(include include/BlasWrapper.h
             include/Common.h
             include/MathConstants.h
             include/Matrix.h
             include/MatrixOperations.h
             include/Tensor.h
             include/TensorOperations.h
             include/Vector.h
             include/VectorOperations.h
)

set(doc doc/README.md)

source_group("src" FILES ${src})
source_group("include" FILES ${include})
source_group("doc" FILES ${doc})

add_library(${library_name} ${src} ${include} ${doc})
target_include_directories(${library_name} PRIVATE include ${ELL_LIBRARIES_DIR})
target_include_directories(${library_name} SYSTEM PUBLIC ${BLAS_INCLUDE_DIRS})
target_link_libraries(${library_name} utilities ${BLAS_LIBS})

if(BLAS_FOUND)
  target_compile_definitions(${library_name} PUBLIC USE_BLAS=1)
endif()

set_property(TARGET ${library_name} PROPERTY FOLDER "libraries")

#
# math test
#

set(test_name ${library_name}_test)

set(test_src test/src/main.cpp)

set(test_include test/include/math_profile.h
                  test/include/Matrix_test.h
                  test/include/Tensor_test.h
                  test/include/Vector_test.h)

source_group("src" FILES ${test_src})
source_group("include" FILES ${test_include})

add_executable(${test_name} ${test_src} ${test_include} ${include})
target_include_directories(${test_name} PRIVATE test/include ${ELL_LIBRARIES_DIR})
target_link_libraries(${test_name} math testing)
copy_shared_libraries(${test_name})

set_property(TARGET ${test_name} PROPERTY FOLDER "tests")

add_test(NAME ${test_name} COMMAND ${test_name})
set_test_library_path(${test_name})

#
# math profile
#

set(profile_name ${library_name}_profile)

set(profile_src test/src/math_profile_main.cpp)

set(profile_include test/include/math_profile.h)

source_group("src" FILES ${profile_src})
source_group("include" FILES ${profile_include})

add_executable(${profile_name} ${profile_src} ${profile_include} ${include})
target_include_directories(${profile_name} PRIVATE test/include ${ELL_LIBRARIES_DIR})
target_link_libraries(${profile_name} math testing)
copy_shared_libraries(${profile_name})

set_property(TARGET ${profile_name} PROPERTY FOLDER "tests")

if (PROFILING)
add_test(NAME ${profile_name} COMMAND ${profile_name} CONFIGURATIONS Release)
set_test_library_path(${profile_name})
endif()
