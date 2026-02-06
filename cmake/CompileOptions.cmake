set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# TTL logging level
if(DEFINED TTL_LOG_MIN_LEVEL)
    add_compile_definitions(TTL_LOG_MIN_LEVEL=${TTL_LOG_MIN_LEVEL})
    message(STATUS "TTL log level: ${TTL_LOG_MIN_LEVEL}")
endif()

# https://clang.llvm.org/docs/DiagnosticsReference.html
add_compile_options(-Wall -Wextra -Wpedantic -g -fno-omit-frame-pointer)

if((CMAKE_BUILD_TYPE MATCHES Release))
    list(APPEND LIBS_LIST "mimalloc")
    message(STATUS "mimalloc: enabled")
endif()


# Turn warnings into errors
add_compile_options(-Wno-language-extension-token)

add_compile_options(-Wno-error=unused-command-line-argument)

add_compile_options(-gdwarf-4)

add_compile_options(-stdlib=libstdc++)
add_link_options(-stdlib=libstdc++)

# fuse
find_library(FUSE3_LIB NAMES fuse3)
if(FUSE3_LIB)
    add_link_options(${FUSE3_LIB})
else()
    message(STATUS "fuse3 not found; skipping link")
endif()

message(STATUS "CPP standard: ${CMAKE_CXX_STANDARD}")
