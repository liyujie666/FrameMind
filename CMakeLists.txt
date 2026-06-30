cmake_minimum_required(VERSION 3.20)

project(FrameMind LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 基本编译设置
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

if(MSVC)
    # 源码按 UTF-8 处理，避免中文注释/字符串告警
    add_compile_options(/utf-8)
endif()

# ---------------------------------------------------------------------------
# Qt6
#   若 CMake 找不到 Qt，请在配置时传入：
#   -DCMAKE_PREFIX_PATH="C:/Qt/6.9.0/msvc2019_64"
# ---------------------------------------------------------------------------
find_package(Qt6 6.9 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Network
    Sql
    Concurrent
    Svg
)

qt_standard_project_setup()

# ---------------------------------------------------------------------------
# SmartPlayer SDK (third_party 预编译产物)
#   头文件 / .lib / .dll 见 third_party/smartplayer_sdk/{include,lib,bin}
# ---------------------------------------------------------------------------
set(SMARTPLAYER_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/smartplayer_sdk")

add_library(smartplayer SHARED IMPORTED)
set_target_properties(smartplayer PROPERTIES
    IMPORTED_IMPLIB   "${SMARTPLAYER_SDK_DIR}/lib/SmartPlayerSDK.lib"
    IMPORTED_LOCATION "${SMARTPLAYER_SDK_DIR}/bin/SmartPlayerSDK.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${SMARTPLAYER_SDK_DIR}/include"
)

# ---------------------------------------------------------------------------
# 源文件
# ---------------------------------------------------------------------------
file(GLOB_RECURSE FRAMEMIND_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
file(GLOB_RECURSE FRAMEMIND_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

set(FRAMEMIND_RESOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/resources.qrc"
)

qt_add_executable(FrameMind WIN32
    ${FRAMEMIND_SOURCES}
    ${FRAMEMIND_HEADERS}
    ${FRAMEMIND_RESOURCES}
)

target_include_directories(FrameMind PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(FrameMind PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::Sql
    Qt6::Concurrent
    Qt6::Svg
    smartplayer
)

# Windows DPAPI（SettingsService 密钥存储）
if(WIN32)
    target_link_libraries(FrameMind PRIVATE Crypt32)
endif()

# ---------------------------------------------------------------------------
# 运行期依赖：把 SmartPlayer SDK + FFmpeg + SDL2 的 dll 拷到可执行目录
# ---------------------------------------------------------------------------
file(GLOB SMARTPLAYER_RUNTIME_DLLS "${SMARTPLAYER_SDK_DIR}/bin/*.dll")
add_custom_command(TARGET FrameMind POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:FrameMind>"
    COMMENT "Copying SmartPlayer runtime dlls"
)
foreach(_dll ${SMARTPLAYER_RUNTIME_DLLS})
    add_custom_command(TARGET FrameMind POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_dll}" "$<TARGET_FILE_DIR:FrameMind>"
    )
endforeach()
