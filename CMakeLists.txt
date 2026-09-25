cmake_minimum_required(VERSION 3.15)
project(HOW C)

add_executable(HOW WIN32
    src/main.c
    src/compat.c
    src/arkrt.c
    src/ui_parse.c
    src/ui_render.c
    src/store.c
    src/abc.c
    src/json.c
    src/selftest.c
    third_party/miniz/miniz.c
    third_party/miniz/miniz_tdef.c
    third_party/miniz/miniz_tinfl.c
    third_party/miniz/miniz_zip.c
)

target_include_directories(HOW PRIVATE src third_party/miniz)
target_compile_definitions(HOW PRIVATE UNICODE _UNICODE)
target_link_libraries(HOW PRIVATE user32 gdi32 comdlg32)

if(MSVC)
    target_compile_options(HOW PRIVATE /utf-8 /W3)
else()
    target_compile_options(HOW PRIVATE -O2 -Wall)
endif()
