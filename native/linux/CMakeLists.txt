set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(ICON_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/icon_assets.h")
file(GLOB_RECURSE ICON_INPUTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/toolbar-icons/*")
add_custom_command(OUTPUT "${ICON_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/build_icon_assets.py" --output "${ICON_HEADER}"
    DEPENDS ${ICON_INPUTS} "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/build_icon_assets.py" "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/icon_raster.py")
find_package(PkgConfig REQUIRED)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
add_library(linux_engine SHARED engine.cpp icons.cpp fonts.cpp "${ICON_HEADER}")
target_include_directories(linux_engine PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(linux_engine PRIVATE pthread dl PkgConfig::FONTCONFIG)
target_compile_options(linux_engine PRIVATE -fvisibility=hidden)
set_target_properties(linux_engine PROPERTIES OUTPUT_NAME v8-chroma-linux)
install(TARGETS linux_engine LIBRARY DESTINATION dist/linux-preview)
enable_language(ASM)
add_executable(linux_injector injector.cpp inject_payload.S)
target_link_libraries(linux_injector PRIVATE dl)
set_target_properties(linux_injector PROPERTIES OUTPUT_NAME v8-chroma-injector)
install(TARGETS linux_injector RUNTIME DESTINATION dist/linux-preview)

option(V8_CHROMA_BUILD_FIXTURES "Build owned process fixtures for tests" ON)
if(V8_CHROMA_BUILD_FIXTURES)
    enable_testing()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)
    # An owned process called 1cv8 exercises the same process-mode guard as 1C.
    add_executable(linux_engine_host host.cpp)
    target_link_libraries(linux_engine_host PRIVATE dl)
    set_target_properties(linux_engine_host PROPERTIES OUTPUT_NAME 1cv8)
    add_library(linux_cairo_fixture SHARED cairo_fixture.cpp)
    target_link_options(linux_cairo_fixture PRIVATE LINKER:--no-as-needed)
    target_link_libraries(linux_cairo_fixture PRIVATE PkgConfig::CAIRO)
    set_target_properties(linux_cairo_fixture PROPERTIES OUTPUT_NAME cairo-v8)
    add_library(linux_binder_fixture SHARED binder_fixture.cpp font_core_fixture.cpp font_core_fixture.S)
    target_compile_options(linux_binder_fixture PRIVATE -fno-optimize-sibling-calls)
    target_link_libraries(linux_binder_fixture PRIVATE dl)
    set_target_properties(linux_binder_fixture PROPERTIES OUTPUT_NAME core83 PREFIX "")
    add_library(linux_graphics_fixture SHARED graphics_fixture.cpp)
    target_link_libraries(linux_graphics_fixture PRIVATE PkgConfig::CAIRO dl linux_binder_fixture)
    set_target_properties(linux_graphics_fixture PROPERTIES OUTPUT_NAME grphcs PREFIX "" BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN")
    set(ICON_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/fixture/icon_assets.h")
    add_custom_command(OUTPUT "${ICON_FIXTURE}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/build_icon_assets.py" --fixture --output "${ICON_FIXTURE}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/build_icon_assets.py" "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/icon_raster.py")
    add_executable(linux_icon_test icon_test.cpp icons.cpp "${ICON_FIXTURE}")
    target_include_directories(linux_icon_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/fixture")
    target_link_libraries(linux_icon_test PRIVATE PkgConfig::CAIRO dl pthread)
    add_test(NAME linux-icons COMMAND linux_icon_test)
    add_executable(linux_editor_refresh_test editor_refresh_test.cpp)
    target_link_libraries(linux_editor_refresh_test PRIVATE dl)
    add_test(NAME linux-editor-refresh COMMAND linux_editor_refresh_test)
    add_executable(linux_trace_test trace_test.cpp)
    target_link_libraries(linux_trace_test PRIVATE pthread dl)
    add_test(NAME linux-trace-recovery COMMAND linux_trace_test)
    set_tests_properties(linux-trace-recovery PROPERTIES TIMEOUT 15)
    add_test(NAME linux-text-rendering COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_text_rendering.py" --expect-no-ligatures)
    add_test(NAME linux-text-ligatures COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_text_rendering.py" --family "Fira Code" --require-ligatures)
    add_test(NAME linux-engine
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_engine.py"
            --engine "$<TARGET_FILE:linux_engine>" --host "$<TARGET_FILE:linux_engine_host>"
            --cairo "$<TARGET_FILE:linux_cairo_fixture>"
            --binder "$<TARGET_FILE:linux_binder_fixture>"
            --goldens "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/golden")
    set_tests_properties(linux-engine PROPERTIES TIMEOUT 60)
    add_test(NAME linux-live-injection
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_injection.py"
            --injector "$<TARGET_FILE:linux_injector>" --engine "$<TARGET_FILE:linux_engine>"
            --host "$<TARGET_FILE:linux_engine_host>" --cairo "$<TARGET_FILE:linux_cairo_fixture>"
            --graphics "$<TARGET_FILE:linux_graphics_fixture>"
            --core "$<TARGET_FILE:linux_binder_fixture>"
            --goldens "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/golden")
    set_tests_properties(linux-live-injection PROPERTIES TIMEOUT 60)
endif()
