cmake_minimum_required(VERSION 3.10)

# 项目名称和版本
project(cmus VERSION 2.12.0)

# 设置C标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 配置选项
option(CONFIG_AAC "Enable AAC support" ON)
option(CONFIG_ALSA "Enable ALSA support" ON)
option(CONFIG_AO "Enable libao support" ON)
option(CONFIG_CDIO "Enable CDIO support" ON)
option(CONFIG_CUE "Enable CUE support" ON)
option(CONFIG_FFMPEG "Enable FFmpeg support" ON)
option(CONFIG_FLAC "Enable FLAC support" ON)
option(CONFIG_JACK "Enable JACK support" ON)
option(CONFIG_MAD "Enable MAD support" ON)
option(CONFIG_MODPLUG "Enable ModPlug support" ON)
option(CONFIG_MPC "Enable MPC support" ON)
option(CONFIG_MPRIS "Enable MPRIS support" ON)
option(CONFIG_OPUS "Enable Opus support" ON)
option(CONFIG_OSS "Enable OSS support" ON)
option(CONFIG_PULSE "Enable PulseAudio support" ON)
option(CONFIG_VORBIS "Enable Vorbis support" ON)
option(CONFIG_WAV "Enable WAV support" ON)
option(CONFIG_WAVPACK "Enable WavPack support" ON)

# 设置编译标志
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wshadow -Wcast-align -Wpointer-arith -Wwrite-strings -Wundef -Wmissing-prototypes -Wredundant-decls -Wextra -Wno-sign-compare -Wformat-security -Wold-style-definition -Wno-pointer-sign -Werror-implicit-function-declaration -Wno-unused-parameter -Wno-missing-field-initializers -D_FILE_OFFSET_BITS=64")

# 设置调试标志
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0 -g")
endif()

# 设置模块标志
set(CMAKE_SHARED_MODULE_PREFIX "")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 查找依赖项
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# 检查ncurses库
pkg_check_modules(NCURSES REQUIRED ncursesw)
include_directories(${NCURSES_INCLUDE_DIRS})

# 其他依赖项
pkg_check_modules(ICONV iconv)
if(ICONV_FOUND)
  set(HAVE_ICONV ON)
endif()

pkg_check_modules(DL dl)
pkg_check_modules(SYSTEMD libsystemd)
pkg_check_modules(DISCID libdiscid)
if(DISCID_FOUND)
  set(HAVE_DISCID ON)
endif()

# 生成版本信息
execute_process(
  COMMAND git describe HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE VERSION
  ERROR_QUIET
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT VERSION)
  execute_process(
    COMMAND git describe --tags HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE VERSION
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
endif()
if(NOT VERSION)
  execute_process(
    COMMAND git rev-parse --verify --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE VERSION
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
endif()
if(NOT VERSION)
  set(VERSION "v${PROJECT_VERSION}")
endif()

# 定义配置变量
set(HAVE_CONFIG ON)

# 配置文件
configure_file(config.h.in ${CMAKE_BINARY_DIR}/config.h)
include_directories(${CMAKE_BINARY_DIR})

# 主可执行文件源文件
set(CMUS_SOURCES
    ape.c browser.c buffer.c cache.c channelmap.c cmdline.c cmus.c command_mode.c
    comment.c convert.c cue.c cue_utils.c debug.c discid.c editable.c expr.c
    filters.c format_print.c gbuf.c glob.c help.c history.c http.c id3.c input.c
    job.c keys.c keyval.c lib.c load_dir.c locking.c mergesort.c misc.c options.c
    output.c pcm.c player.c play_queue.c pl.c pl_env.c pinyin_search.c rbtree.c read_wrapper.c
    search_mode.c search.c server.c spawn.c tabexp_file.c tabexp.c track_info.c
    track.c tree.c uchar.c u_collate.c ui_curses.c window.c worker.c xstrjoin.c
    file.c path.c prog.c xmalloc.c
)

# cmus-remote源文件
set(CMUS_REMOTE_SOURCES
    main.c file.c misc.c path.c prog.c xmalloc.c xstrjoin.c
)

# 添加主可执行文件
add_executable(cmus ${CMUS_SOURCES})
target_include_directories(cmus PRIVATE .)
target_compile_definitions(cmus PRIVATE 
    VERSION="${VERSION}" 
    DEFAULT_PORT=3000
    HAVE_DISCID=$<BOOL:${HAVE_DISCID}>
    HAVE_CONFIG=$<BOOL:${HAVE_CONFIG}>
    HAVE_ICONV=$<BOOL:${HAVE_ICONV}>
)

# 添加cmus-remote可执行文件
add_executable(cmus-remote ${CMUS_REMOTE_SOURCES})
target_include_directories(cmus-remote PRIVATE .)
target_compile_definitions(cmus-remote PRIVATE 
    VERSION="${VERSION}" 
    DEFAULT_PORT=3000
)

# 链接库
target_link_libraries(cmus 
    ${NCURSES_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
    ${ICONV_LIBRARIES}
    ${DL_LIBRARIES}
    ${DISCID_LIBRARIES}
    m
    ${SYSTEMD_LIBRARIES}
)

target_link_libraries(cmus-remote
    ${CMAKE_THREAD_LIBS_INIT}
    ${ICONV_LIBRARIES}
)

# 添加调试目标包装器
add_custom_target(debug_wrapper DEPENDS cmus
    COMMENT "Debug target ready"
)

# 设置安装路径
include(GNUInstallDirs)
install(TARGETS cmus cmus-remote
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# 创建插件目标
add_custom_target(plugins DEPENDS ip_plugins op_plugins)
add_custom_target(ip_plugins)
add_custom_target(op_plugins)

# 声明可用的输入插件
set(IP_PLUGINS)

# CDIO插件
if(CONFIG_CDIO)
    pkg_check_modules(CDIO libcdio libcdio_cdda)
    if(CDIO_FOUND)
        add_library(ip_cdio MODULE ip/cdio.c)
        target_include_directories(ip_cdio PRIVATE . ${CDIO_INCLUDE_DIRS})
        target_link_libraries(ip_cdio ${CDIO_LIBRARIES})
        set_target_properties(ip_cdio PROPERTIES OUTPUT_NAME "cdio" SUFFIX ".so")
        list(APPEND IP_PLUGINS ip_cdio)
    endif()
endif()

# FLAC插件
if(CONFIG_FLAC)
    pkg_check_modules(FLAC flac)
    if(FLAC_FOUND)
        add_library(ip_flac MODULE ip/flac.c)
        target_include_directories(ip_flac PRIVATE . ${FLAC_INCLUDE_DIRS})
        target_link_libraries(ip_flac ${FLAC_LIBRARIES})
        set_target_properties(ip_flac PROPERTIES OUTPUT_NAME "flac" SUFFIX ".so")
        list(APPEND IP_PLUGINS ip_flac)
    endif()
endif()

# MAD插件
if(CONFIG_MAD)
    pkg_check_modules(MAD mad)
    if(MAD_FOUND)
        add_library(ip_mad MODULE ip/mad.c ip/nomad.c)
        target_include_directories(ip_mad PRIVATE . ${MAD_INCLUDE_DIRS})
        target_link_libraries(ip_mad ${MAD_LIBRARIES} m)
        set_target_properties(ip_mad PROPERTIES OUTPUT_NAME "mad" SUFFIX ".so")
        list(APPEND IP_PLUGINS ip_mad)
    endif()
endif()

# MODPLUG插件
if(CONFIG_MODPLUG)
    pkg_check_modules(MODPLUG libmodplug)
    if(MODPLUG_FOUND)
        add_library(ip_modplug MODULE ip/modplug.c)
        target_include_directories(ip_modplug PRIVATE . ${MODPLUG_INCLUDE_DIRS})
        target_link_libraries(ip_modplug ${MODPLUG_LIBRARIES})
        set_target_properties(ip_modplug PROPERTIES OUTPUT_NAME "modplug" SUFFIX ".so")
        list(APPEND IP_PLUGINS ip_modplug)
    endif()
endif()

# MPC插件
if(CONFIG_MPC)
    pkg_check_modules(MPC libmpcdec)
    if(MPC_FOUND)
        add_library(ip_mpc MODULE ip/mpc.c)
        target_include_directories(ip_mpc PRIVATE . ${MPC_INCLUDE_DIRS})
        target_link_libraries(ip_mpc ${MPC_LIBRARIES} m)
        set_target_properties(ip_mpc PROPERTIES OUTPUT_NAME "mpc" SUFFIX ".so")
        list(APPEND IP_PLUGINS ip_mpc)
    endif()
endif()

# 更多输入插件...
if(CONFIG_VORBIS)
    pkg_check_modules(VORBIS vorbisfile)
    if(VORBIS_FOUND)
        add_library(ip_vorbis MODULE ip/vorbis.c)
        target_include_directories(ip_vorbis PRIVATE . ${VORBIS_INCLUDE_DIRS})
        target_link_libraries(ip_vorbis ${VORBIS_LIBRARIES} m)
        set_target_properties(ip_vorbis PROPERTIES OUTPUT_NAME "vorbis" SUFFIX ".so")
        list(APPEND IP_PLUGINS ip_vorbis)
    endif()
endif()

if(CONFIG_OPUS)
    pkg_check_modules(OPUS opusfile)
    if(OPUS_FOUND)
        add_library(ip_opus MODULE ip/opus.c)
        target_include_directories(ip_opus PRIVATE . ${OPUS_INCLUDE_DIRS})
        target_link_libraries(ip_opus ${OPUS_LIBRARIES})
        set_target_properties(ip_opus PROPERTIES OUTPUT_NAME "opus" SUFFIX ".so")
        list(APPEND IP_PLUGINS ip_opus)
    endif()
endif()

if(CONFIG_WAVPACK)
    pkg_check_modules(WAVPACK wavpack)
    if(WAVPACK_FOUND)
        add_library(ip_wavpack MODULE ip/wavpack.c)
        target_include_directories(ip_wavpack PRIVATE . ${WAVPACK_INCLUDE_DIRS})
        target_link_libraries(ip_wavpack ${WAVPACK_LIBRARIES})
        set_target_properties(ip_wavpack PROPERTIES OUTPUT_NAME "wavpack" SUFFIX ".so")
        list(APPEND IP_PLUGINS ip_wavpack)
    endif()
endif()

if(CONFIG_WAV)
    add_library(ip_wav MODULE ip/wav.c)
    target_include_directories(ip_wav PRIVATE .)
    set_target_properties(ip_wav PROPERTIES OUTPUT_NAME "wav" SUFFIX ".so")
    list(APPEND IP_PLUGINS ip_wav)
endif()

if(CONFIG_FFMPEG)
    pkg_check_modules(FFMPEG libavformat libavcodec libswresample libavutil)
    if(FFMPEG_FOUND)
        add_library(ip_ffmpeg MODULE ip/ffmpeg.c)
        target_include_directories(ip_ffmpeg PRIVATE . ${FFMPEG_INCLUDE_DIRS})
        target_link_libraries(ip_ffmpeg ${FFMPEG_LIBRARIES})
        set_target_properties(ip_ffmpeg PROPERTIES OUTPUT_NAME "ffmpeg" SUFFIX ".so")
        list(APPEND IP_PLUGINS ip_ffmpeg)
    endif()
endif()

if(CONFIG_AAC)
    pkg_check_modules(AAC faad2)
    if(AAC_FOUND)
        add_library(ip_aac MODULE ip/aac.c)
        target_include_directories(ip_aac PRIVATE . ${AAC_INCLUDE_DIRS})
        target_link_libraries(ip_aac ${AAC_LIBRARIES})
        set_target_properties(ip_aac PROPERTIES OUTPUT_NAME "aac" SUFFIX ".so")
        list(APPEND IP_PLUGINS ip_aac)
    endif()
endif()

if(CONFIG_CUE)
    add_library(ip_cue MODULE ip/cue.c)
    target_include_directories(ip_cue PRIVATE .)
    target_link_libraries(ip_cue m)
    set_target_properties(ip_cue PROPERTIES OUTPUT_NAME "cue" SUFFIX ".so")
    list(APPEND IP_PLUGINS ip_cue)
endif()

# 将所有输入插件添加为ip_plugins的依赖
add_dependencies(ip_plugins ${IP_PLUGINS})

# 声明可用的输出插件
set(OP_PLUGINS)

# ALSA输出插件
if(CONFIG_ALSA)
    pkg_check_modules(ALSA alsa)
    if(ALSA_FOUND)
        add_library(op_alsa MODULE op/alsa.c op/mixer_alsa.c)
        target_include_directories(op_alsa PRIVATE . ${ALSA_INCLUDE_DIRS})
        target_link_libraries(op_alsa ${ALSA_LIBRARIES})
        set_target_properties(op_alsa PROPERTIES OUTPUT_NAME "alsa" SUFFIX ".so")
        list(APPEND OP_PLUGINS op_alsa)
    endif()
endif()

# PulseAudio输出插件
if(CONFIG_PULSE)
    pkg_check_modules(PULSE libpulse)
    if(PULSE_FOUND)
        add_library(op_pulse MODULE op/pulse.c)
        target_include_directories(op_pulse PRIVATE . ${PULSE_INCLUDE_DIRS})
        target_link_libraries(op_pulse ${PULSE_LIBRARIES})
        target_compile_definitions(op_pulse PRIVATE VERSION="${VERSION}")
        set_target_properties(op_pulse PROPERTIES OUTPUT_NAME "pulse" SUFFIX ".so")
        list(APPEND OP_PLUGINS op_pulse)
    endif()
endif()

# OSS输出插件
if(CONFIG_OSS)
    add_library(op_oss MODULE op/oss.c op/mixer_oss.c)
    target_include_directories(op_oss PRIVATE .)
    set_target_properties(op_oss PROPERTIES OUTPUT_NAME "oss" SUFFIX ".so")
    list(APPEND OP_PLUGINS op_oss)
endif()

# JACK输出插件
if(CONFIG_JACK)
    pkg_check_modules(JACK jack)
    if(JACK_FOUND)
        add_library(op_jack MODULE op/jack.c)
        target_include_directories(op_jack PRIVATE . ${JACK_INCLUDE_DIRS})
        target_link_libraries(op_jack ${JACK_LIBRARIES})
        set_target_properties(op_jack PROPERTIES OUTPUT_NAME "jack" SUFFIX ".so")
        list(APPEND OP_PLUGINS op_jack)
    endif()
endif()

# AO输出插件
if(CONFIG_AO)
    pkg_check_modules(AO ao)
    if(AO_FOUND)
        add_library(op_ao MODULE op/ao.c)
        target_include_directories(op_ao PRIVATE . ${AO_INCLUDE_DIRS})
        target_link_libraries(op_ao ${AO_LIBRARIES})
        set_target_properties(op_ao PROPERTIES OUTPUT_NAME "ao" SUFFIX ".so")
        list(APPEND OP_PLUGINS op_ao)
    endif()
endif()

# 将所有输出插件添加为op_plugins的依赖
add_dependencies(op_plugins ${OP_PLUGINS})

# 安装插件
install(TARGETS ${IP_PLUGINS}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmus/ip
)

install(TARGETS ${OP_PLUGINS}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmus/op
) 