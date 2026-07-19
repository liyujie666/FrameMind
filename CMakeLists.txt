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
#   CMake 自动查找系统中的 Qt，或通过 -DCMAKE_PREFIX_PATH 指定
# ---------------------------------------------------------------------------
# 尝试从常见位置查找 Qt
set(QT_PATHS
    "D:/Qt/6.9.1/msvc2022_64"
    "D:/Qt/6.9.0/msvc2022_64"
    "C:/Qt/6.9.1/msvc2022_64"
    "C:/Qt/6.9.0/msvc2022_64"
)

foreach(path IN LISTS QT_PATHS)
    if(EXISTS "${path}/lib/cmake/Qt6")
        list(APPEND CMAKE_PREFIX_PATH "${path}")
        break()
    endif()
endforeach()

find_package(Qt6 6.9 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Network
    Sql
    Concurrent
    Svg
    OpenGL
    OpenGLWidgets
)

qt_standard_project_setup()

# ---------------------------------------------------------------------------
# SmartPlayer SDK (third_party 预编译产物)
#   头文件 / .lib / .dll 见 third_party/smartplayer_sdk/{include,lib,bin}
#
#   ⚠️ SmartPlayerSDK.dll 的导入表写死了对 FFmpeg 子库的具体版本号依赖
#   （如 avfilter-10.dll / avcodec-61.dll 等）。third_party/smartplayer_sdk/bin/
#   必须放入与之版本号完全匹配的 FFmpeg dll，否则运行时会出现
#   "无法定位程序输入点 xxx 于动态链接库" 错误。
#   下面用 dumpbin 解析 SmartPlayerSDK.dll 的依赖，自动校验配套 dll 是否齐全。
# ---------------------------------------------------------------------------
set(SMARTPLAYER_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/smartplayer_sdk")

add_library(smartplayer SHARED IMPORTED)
set_target_properties(smartplayer PROPERTIES
    IMPORTED_IMPLIB   "${SMARTPLAYER_SDK_DIR}/lib/SmartPlayerSDK.lib"
    IMPORTED_LOCATION "${SMARTPLAYER_SDK_DIR}/bin/SmartPlayerSDK.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${SMARTPLAYER_SDK_DIR}/include"
)

# 运行时依赖校验：解析 SmartPlayerSDK.dll 导入表，确认 bin/ 下有匹配的 FFmpeg dll
# 仅在 Windows + 找到 dumpbin 时执行（开发期防呆，跳过不影响构建）
if(WIN32)
    find_program(DUMPBIN_EXE dumpbin
        HINTS
        "$ENV{VSINSTALLDIR}/VC/Tools/MSVC/*/bin/Hostx64/x64"
        "$ENV{VCToolsInstallDir}/bin/Hostx64/x64"
        "C:/Program Files/Microsoft Visual Studio/2022/*/VC/Tools/MSVC/*/bin/Hostx64/x64"
        "C:/Program Files (x86)/Microsoft Visual Studio/2022/*/VC/Tools/MSVC/*/bin/Hostx64/x64"
    )
    if(DUMPBIN_EXE)
        set(SMARTPLAYER_DLL "${SMARTPLAYER_SDK_DIR}/bin/SmartPlayerSDK.dll")
        if(EXISTS "${SMARTPLAYER_DLL}")
            execute_process(
                COMMAND "${DUMPBIN_EXE}" /dependents "${SMARTPLAYER_DLL}"
                OUTPUT_VARIABLE SMARTPLAYER_DEPS_RAW
                ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            # 提取 FFmpeg 相关依赖（av*.dll / sw*.dll / SDL2.dll）
            string(REGEX REPLACE "(\r\n|\n)" ";" SMARTPLAYER_DEPS_LIST "${SMARTPLAYER_DEPS_RAW}")
            set(SMARTPLAYER_FFMPEG_DEPS "")
            foreach(_dep ${SMARTPLAYER_DEPS_LIST})
                string(STRIP "${_dep}" _dep)
                if(_dep MATCHES "^(av[a-z]+-[0-9]+\\.dll|sw[a-z]+-[0-9]+\\.dll|SDL2\\.dll)$")
                    list(APPEND SMARTPLAYER_FFMPEG_DEPS "${_dep}")
                endif()
            endforeach()

            # 校验每个依赖 dll 是否存在于 bin/
            set(SMARTPLAYER_MISSING_DLLS "")
            foreach(_dep ${SMARTPLAYER_FFMPEG_DEPS})
                if(NOT EXISTS "${SMARTPLAYER_SDK_DIR}/bin/${_dep}")
                    list(APPEND SMARTPLAYER_MISSING_DLLS "${_dep}")
                endif()
            endforeach()

            if(SMARTPLAYER_MISSING_DLLS)
                message(WARNING
                    "SmartPlayerSDK.dll 依赖的 FFmpeg dll 在 ${SMARTPLAYER_SDK_DIR}/bin/ 缺失或不匹配:\n"
                    "  缺失: ${SMARTPLAYER_MISSING_DLLS}\n"
                    "  SmartPlayerSDK.dll 实际需要的版本（来自导入表）: ${SMARTPLAYER_FFMPEG_DEPS}\n"
                    "  请从 SmartPlayerSDK 原始发布包中提取配套的 FFmpeg dll 放入 bin/。\n"
                    "  ⚠️ 不要用 D:/ffmpeg 等系统 PATH 里的 FFmpeg 替代 —— 内部 ABI 不兼容。"
                )
            else()
                message(STATUS "SmartPlayerSDK FFmpeg 依赖校验通过: ${SMARTPLAYER_FFMPEG_DEPS}")
            endif()
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# 源文件
# ---------------------------------------------------------------------------
file(GLOB_RECURSE FRAMEMIND_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
file(GLOB_RECURSE FRAMEMIND_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

# ---------------------------------------------------------------------------
# Video RAG 小模型集成（可选）
#   third_party/{onnxruntime,whisper.cpp} 预编译产物就位后默认开启。
#   若尚未放置依赖可手动关闭：cmake -DFRAMEMIND_ENABLE_ONNX=OFF ..
# ---------------------------------------------------------------------------
option(FRAMEMIND_ENABLE_ONNX    "Enable ONNX Runtime (CLIP/BGE/TransNetV2)" ON)
option(FRAMEMIND_ENABLE_WHISPER "Enable whisper.cpp speech-to-text"          ON)
option(FRAMEMIND_ENABLE_FAISS   "Enable FAISS in-process vector index"       OFF)

# ONNX Runtime 未启用时，排除依赖它的源文件
if(NOT FRAMEMIND_ENABLE_ONNX)
    list(FILTER FRAMEMIND_SOURCES EXCLUDE REGEX
        "onnx_runtime_engine\\.cpp|clip_service\\.cpp|embedding_service\\.cpp")
endif()

# whisper.cpp 未启用时，排除依赖它的源文件
if(NOT FRAMEMIND_ENABLE_WHISPER)
    list(FILTER FRAMEMIND_SOURCES EXCLUDE REGEX "whisper_service\\.cpp")
endif()

# FAISS 未启用时，排除依赖它的源文件
if(NOT FRAMEMIND_ENABLE_FAISS)
    list(FILTER FRAMEMIND_SOURCES EXCLUDE REGEX "faiss_index\\.cpp")
endif()

set(FRAMEMIND_RESOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/resources.qrc"
)

qt_add_executable(FrameMind WIN32
    ${FRAMEMIND_SOURCES}
    ${FRAMEMIND_HEADERS}
    ${FRAMEMIND_RESOURCES}
)

# 设置 Windows 任务栏图标
if(WIN32)
    set(APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/app_dark.ico")
    if(EXISTS "${APP_ICON}")
        set_target_properties(FrameMind PROPERTIES
            WIN32_EXECUTABLE TRUE
            ICON "${APP_ICON}"
        )
    endif()
endif()

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
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    smartplayer
)

# ---------------------------------------------------------------------------
# FFmpeg 链接（AudioDecoder 音频解码依赖）
#   布局：third_party/ffmpeg/{include,lib,bin}
#     include/  → libavcodec/ libavformat/ libavutil/ libswresample/ 等
#     lib/      → avcodec.lib avformat.lib avutil.lib swresample.lib 等
#     bin/      → avcodec-61.dll avformat-61.dll avutil-59.dll swresample-5.dll 等
#
#   注意：这些 DLL 与 SmartPlayerSDK 共用同一套 FFmpeg 构建产物，
#         版本必须与 SmartPlayerSDK.dll 导入表一致。
# ---------------------------------------------------------------------------
set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg")

target_include_directories(FrameMind PRIVATE "${FFMPEG_ROOT}/include")

# 需要链接的 FFmpeg 子库（AudioDecoder 用到 avformat + avcodec + avutil + swresample）
set(FFMPEG_LIBS avformat avcodec avutil swresample)
foreach(_lib ${FFMPEG_LIBS})
    add_library(ffmpeg_${_lib} SHARED IMPORTED)
    # 查找 bin/ 下的对应 DLL（文件名含版本号，用 GLOB 匹配）
    file(GLOB _dll "${FFMPEG_ROOT}/bin/${_lib}*.dll")
    list(GET _dll 0 _dll_path)
    set_target_properties(ffmpeg_${_lib} PROPERTIES
        IMPORTED_IMPLIB   "${FFMPEG_ROOT}/lib/${_lib}.lib"
        IMPORTED_LOCATION "${_dll_path}"
    )
    target_link_libraries(FrameMind PRIVATE ffmpeg_${_lib})
endforeach()

# 运行时 DLL 拷贝（bin/ 下所有 .dll，含 SDL2）
file(GLOB FFMPEG_RUNTIME_DLLS "${FFMPEG_ROOT}/bin/*.dll")
foreach(_dll ${FFMPEG_RUNTIME_DLLS})
    add_custom_command(TARGET FrameMind POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_dll}" "$<TARGET_FILE_DIR:FrameMind>"
    )
endforeach()

# ---------------------------------------------------------------------------
# ONNX Runtime 链接（FRAMEMIND_ENABLE_ONNX=ON 时生效）
#   布局：third_party/onnxruntime/{include,lib,bin}
#     include/  → onnxruntime_cxx_api.h 等头文件
#     lib/      → onnxruntime.lib + onnxruntime_providers_shared.lib
#     bin/      → onnxruntime.dll + onnxruntime_providers_shared.dll
#   下载 onnxruntime-win-x64-1.18.1.zip，按上述结构整理即可。
#
#   ⚠️ onnxruntime_providers_shared 必须与主库一同链接：
#      OnnxRuntimeEngine 在初始化 CUDA / CoreML EP 时会从该 DLL 中
#      解析共享符号（OrtGetApiBase 等），链接缺失会导致运行时
#      "找不到入口点" 错误，即使只使用 CPU EP 也需要该 DLL 存在于
#      可执行目录。
# ---------------------------------------------------------------------------
if(FRAMEMIND_ENABLE_ONNX)
    set(ONNXRUNTIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime")

    add_library(onnxruntime SHARED IMPORTED)
    set_target_properties(onnxruntime PROPERTIES
        IMPORTED_IMPLIB   "${ONNXRUNTIME_ROOT}/lib/onnxruntime.lib"
        IMPORTED_LOCATION "${ONNXRUNTIME_ROOT}/bin/onnxruntime.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_ROOT}/include"
    )

    # providers_shared：EP 共享符号库，onnxruntime.dll 的运行时依赖
    if(EXISTS "${ONNXRUNTIME_ROOT}/lib/onnxruntime_providers_shared.lib")
        add_library(onnxruntime_providers_shared SHARED IMPORTED)
        set_target_properties(onnxruntime_providers_shared PROPERTIES
            IMPORTED_IMPLIB   "${ONNXRUNTIME_ROOT}/lib/onnxruntime_providers_shared.lib"
            IMPORTED_LOCATION "${ONNXRUNTIME_ROOT}/bin/onnxruntime_providers_shared.dll"
        )
        set(_ORT_PROVIDERS_SHARED_FOUND TRUE)
    else()
        message(STATUS "onnxruntime_providers_shared.lib not found, skipping (CPU-only build)")
        set(_ORT_PROVIDERS_SHARED_FOUND FALSE)
    endif()

    target_compile_definitions(FrameMind PRIVATE FRAMEMIND_HAS_ONNXRUNTIME)
    target_link_libraries(FrameMind PRIVATE onnxruntime)
    if(_ORT_PROVIDERS_SHARED_FOUND)
        target_link_libraries(FrameMind PRIVATE onnxruntime_providers_shared)
    endif()

    # 运行时 DLL 拷贝（bin/ 下所有 .dll，含 providers_shared.dll）
    file(GLOB ONNXRUNTIME_RUNTIME_DLLS "${ONNXRUNTIME_ROOT}/bin/*.dll")
    foreach(_dll ${ONNXRUNTIME_RUNTIME_DLLS})
        add_custom_command(TARGET FrameMind POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_dll}" "$<TARGET_FILE_DIR:FrameMind>"
        )
    endforeach()
endif()

# ---------------------------------------------------------------------------
# whisper.cpp 链接（FRAMEMIND_ENABLE_WHISPER=ON 时生效）
#   支持两种安装方式：
#     A. 源码版：third_party/whisper.cpp/CMakeLists.txt 存在
#        → add_subdirectory 一起编译
#     B. 预编译版：third_party/whisper.cpp/{include,lib,bin}（无 CMakeLists.txt）
#        → IMPORTED library 直接链接 .lib
#   布局（预编译版，与 smartplayer_sdk / onnxruntime 统一）：
#     include/  → whisper.h + ggml*.h
#     lib/      → whisper.lib + ggml*.lib
#     bin/      → whisper.dll + ggml*.dll
# ---------------------------------------------------------------------------
if(FRAMEMIND_ENABLE_WHISPER)
    set(WHISPER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/whisper.cpp")

    if(EXISTS "${WHISPER_ROOT}/CMakeLists.txt")
        # 方式 A：源码版
        message(STATUS "whisper.cpp: source build (add_subdirectory)")
        set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        add_subdirectory(third_party/whisper.cpp)
        target_link_libraries(FrameMind PRIVATE whisper)
    elseif(EXISTS "${WHISPER_ROOT}/lib/whisper.lib")
        # 方式 B：预编译版
        message(STATUS "whisper.cpp: prebuilt (IMPORTED library)")

        # 辅助函数：注册一个 IMPORTED SHARED 库
        function(add_whisper_imported_lib name)
            add_library(${name} SHARED IMPORTED)
            set_target_properties(${name} PROPERTIES
                IMPORTED_IMPLIB   "${WHISPER_ROOT}/lib/${name}.lib"
                IMPORTED_LOCATION "${WHISPER_ROOT}/bin/${name}.dll"
            )
        endfunction()

        add_whisper_imported_lib(whisper)
        set_target_properties(whisper PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${WHISPER_ROOT}/include"
        )
        add_whisper_imported_lib(ggml)
        add_whisper_imported_lib(ggml-base)
        add_whisper_imported_lib(ggml-cpu)

        target_link_libraries(FrameMind PRIVATE whisper ggml ggml-base ggml-cpu)

        # 可选：CUDA 加速（lib/ggml-cuda.lib 存在时才链接）
        if(EXISTS "${WHISPER_ROOT}/lib/ggml-cuda.lib")
            add_whisper_imported_lib(ggml-cuda)
            target_link_libraries(FrameMind PRIVATE ggml-cuda)
        endif()

        # 运行时 DLL 拷贝（bin/ 下所有 .dll）
        file(GLOB WHISPER_RUNTIME_DLLS "${WHISPER_ROOT}/bin/*.dll")
        foreach(_dll ${WHISPER_RUNTIME_DLLS})
            add_custom_command(TARGET FrameMind POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_dll}" "$<TARGET_FILE_DIR:FrameMind>"
            )
        endforeach()
    else()
        message(FATAL_ERROR
            "whisper.cpp not found.\n"
            "Please either:\n"
            "  A. git clone https://github.com/ggerganov/whisper.cpp third_party/whisper.cpp\n"
            "  B. Place prebuilt include/ + lib/*.lib + bin/*.dll under third_party/whisper.cpp/")
    endif()

    target_compile_definitions(FrameMind PRIVATE FRAMEMIND_HAS_WHISPER)
endif()

# ---------------------------------------------------------------------------
# FAISS 向量索引（FRAMEMIND_ENABLE_FAISS=ON 时生效）
#   默认 OFF：VideoRAGStore 使用内存线性搜索（单视频 < 5000 帧足够）。
#   当需要支持大规模视频库或多视频同时检索时启用。
#
#   布局：third_party/faiss/{include,lib,bin}
#     include/faiss/  → faiss/Index.h, faiss/IndexFlat.h 等
#     lib/            → faiss.lib
#     bin/            → faiss.dll（若使用 DLL 构建；静态库则 bin/ 可留空）
#
#   推荐构建方式（CPU 静态库，最简单）：
#     git clone https://github.com/facebookresearch/faiss third_party/faiss-src
#     cmake -B faiss-build -DFAISS_ENABLE_GPU=OFF -DFAISS_ENABLE_PYTHON=OFF \
#           -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF third_party/faiss-src
#     cmake --build faiss-build --config Release
#   然后将产物整理到 third_party/faiss/{include,lib}。
#
#   也可使用 vcpkg：vcpkg install faiss:x64-windows-static
# ---------------------------------------------------------------------------
if(FRAMEMIND_ENABLE_FAISS)
    set(FAISS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/faiss")

    if(NOT EXISTS "${FAISS_ROOT}/include/faiss/Index.h")
        message(FATAL_ERROR
            "FAISS headers not found at ${FAISS_ROOT}/include/faiss/.\n"
            "Please build or install FAISS and place headers/libs under third_party/faiss/.\n"
            "See CMakeLists.txt comments for build instructions.\n"
            "Or disable: cmake -DFRAMEMIND_ENABLE_FAISS=OFF ..")
    endif()

    # 优先静态库，其次动态库
    if(EXISTS "${FAISS_ROOT}/lib/faiss.lib")
        add_library(faiss STATIC IMPORTED)
        set_target_properties(faiss PROPERTIES
            IMPORTED_LOCATION "${FAISS_ROOT}/lib/faiss.lib"
            INTERFACE_INCLUDE_DIRECTORIES "${FAISS_ROOT}/include"
        )
        # 静态 FAISS 依赖 OpenBLAS 或 MKL；优先查找 OpenBLAS
        find_library(OPENBLAS_LIB NAMES openblas libopenblas
            HINTS "${FAISS_ROOT}/lib" "${FAISS_ROOT}/../openblas/lib")
        if(OPENBLAS_LIB)
            target_link_libraries(faiss INTERFACE "${OPENBLAS_LIB}")
        else()
            message(WARNING
                "OpenBLAS not found. FAISS static build may fail to link.\n"
                "Place openblas.lib alongside faiss.lib, or use a shared FAISS build.")
        endif()
    elseif(EXISTS "${FAISS_ROOT}/lib/faiss_c.lib" OR EXISTS "${FAISS_ROOT}/bin/faiss.dll")
        add_library(faiss SHARED IMPORTED)
        set_target_properties(faiss PROPERTIES
            IMPORTED_IMPLIB   "${FAISS_ROOT}/lib/faiss.lib"
            IMPORTED_LOCATION "${FAISS_ROOT}/bin/faiss.dll"
            INTERFACE_INCLUDE_DIRECTORIES "${FAISS_ROOT}/include"
        )
        # 运行时 DLL 拷贝
        add_custom_command(TARGET FrameMind POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${FAISS_ROOT}/bin/faiss.dll" "$<TARGET_FILE_DIR:FrameMind>"
        )
    else()
        message(FATAL_ERROR
            "FAISS library not found in ${FAISS_ROOT}/lib/.\n"
            "Expected faiss.lib (static) or faiss.lib + bin/faiss.dll (shared).")
    endif()

    target_compile_definitions(FrameMind PRIVATE FRAMEMIND_HAS_FAISS)
    target_link_libraries(FrameMind PRIVATE faiss)
endif()

# Windows DPAPI（SettingsService 密钥存储）
if(WIN32)
    target_link_libraries(FrameMind PRIVATE Crypt32)
endif()

# ---------------------------------------------------------------------------
# 运行期依赖：把 SmartPlayerSDK.dll 拷到可执行目录
#   FFmpeg / SDL2 DLL 已由上面 FFmpeg 块统一拷贝（third_party/ffmpeg/bin/）
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
