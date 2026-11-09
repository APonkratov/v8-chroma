include(FetchContent)

# Static libraries keep the Windows engine self-contained at runtime. Suppress
# the dependency projects' standalone tools, tests and install rules.
set(BUILD_SHARED_LIBS OFF)
set(BUILD_TEST OFF)
set(INSTALL_DOCUMENTATION OFF)
set(MSVC_STATIC_RUNTIME ON)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
FetchContent_Declare(quickjs
    URL https://codeload.github.com/quickjs-ng/quickjs/tar.gz/v0.10.1
    URL_HASH SHA256=4e917eea3bb6051a0551fdd3aac45199ff705fa08920c15b824a08c5fbb032e4
    SOURCE_SUBDIR unused)
FetchContent_Declare(oniguruma
    URL https://codeload.github.com/kkos/oniguruma/tar.gz/v6.9.10
    URL_HASH SHA256=ad92309d0d13eebc27f6592e875f3efbfa3dda2bf6da5952e00f0a2120c921a8
    SOURCE_SUBDIR unused)
FetchContent_Declare(minhook
    URL https://codeload.github.com/TsudaKageyu/minhook/tar.gz/v1.3.4
    URL_HASH SHA256=1aebeae4ca898330c507860acc2fca2eb335fe446a3a2b8444c3bf8b2660a14e
    SOURCE_SUBDIR unused)
FetchContent_MakeAvailable(quickjs oniguruma minhook)
foreach(dependency quickjs oniguruma minhook)
    add_subdirectory("${${dependency}_SOURCE_DIR}" "${${dependency}_BINARY_DIR}" EXCLUDE_FROM_ALL)
endforeach()

set(SYNTAX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../assets/syntax")
set(SYNTAX_INPUTS "${SYNTAX_DIR}/vscode-textmate.js" "${CMAKE_CURRENT_SOURCE_DIR}/src/textmate.js"
    "${SYNTAX_DIR}/1c.tmLanguage.json" "${SYNTAX_DIR}/1c-query.tmLanguage.json")
set(SYNTAX_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/textmate_assets.h")
add_custom_command(OUTPUT "${SYNTAX_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/embed_textmate.py"
        "${SYNTAX_HEADER}" ${SYNTAX_INPUTS}
    DEPENDS ${SYNTAX_INPUTS} ../scripts/embed_textmate.py
    VERBATIM)
add_library(textmate STATIC src/textmate.cpp "${SYNTAX_HEADER}")
target_include_directories(textmate PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(textmate PRIVATE qjs onig)
