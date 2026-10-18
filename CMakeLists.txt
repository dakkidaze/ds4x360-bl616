cmake_minimum_required(VERSION 3.15)

include(proj.conf)

# R09: the product ships USB Full-Speed only; HS is not a supported release
# capability. Always drop any inherited CONFIG_USB_HS (proj.conf, cache or
# parent scope) so the SDK never enables the High-Speed PHY, regardless of
# the FORCE_FS environment variable.
unset(CONFIG_USB_HS)
unset(CONFIG_USB_HS CACHE)
if(DEFINED CONFIG_USB_HS)
    message(FATAL_ERROR
        "R09: Full-Speed only — CONFIG_USB_HS is still set after being "
        "dropped. HS is not a supported release capability; remove the "
        "setting that reintroduces it.")
endif()

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

# Incoming Bluetooth HCI/L2CAP reports must preempt USB HID forwarding.
sdk_add_compile_definitions(-DCONFIG_BT_RX_PRIO=5)

sdk_set_main_file(src/main.c)

target_sources(app PRIVATE
    src/bt_hid_host.c
    src/ds4_protocol.c
    src/usb_gamepad.c
    src/ds4_audio_proto.c
    src/ds4_usb_audio.c
    src/ds4_audio.c
    src/led_status.c
    src/config.c
    src/log_event.c
)

# ── Feature flags (environment variables) ─────────────────────────────
#   GAMEPAD    : on by default; set GAMEPAD=0 to omit the HID gamepad
#   AUDIO      : on by default. The no-argument build is the release candidate
#                and a real wired DS4 has audio, so the default persona is
#                audio + gamepad. Set AUDIO=0 to omit the UAC1 audio function;
#                that is a diagnostic opt-out, not a release variant.
#                Only the literal value 0 disables it (unset = enabled).
#   TOUCHPAD=1 : reserve a build that maps the touchpad to a HID mouse
#                (flag only — no behaviour implemented yet)
#   GYRO=1     : reserve a build that maps gyro to a HID device
#                (flag only — no behaviour implemented yet)
if("$ENV{GAMEPAD}" STREQUAL "0")
    add_compile_definitions(DS4_NO_GAMEPAD=1)
    target_compile_definitions(app PRIVATE DS4_NO_GAMEPAD=1)
    message(STATUS "Feature GAMEPAD: disabled")
else()
    message(STATUS "Feature GAMEPAD: enabled (default)")
endif()
if(NOT "$ENV{AUDIO}" STREQUAL "0")
    # Audio keeps the real-DS4 endpoint layout: speaker OUT 0x01, mic IN 0x82.
    add_compile_definitions(DS4_FEATURE_AUDIO=1)
    target_compile_definitions(app PRIVATE DS4_FEATURE_AUDIO=1)
    message(STATUS "Feature AUDIO: enabled by default (release build)")
    message(STATUS "               set AUDIO=0 only to opt out for diagnostics")
else()
    message(STATUS "Feature AUDIO: disabled (AUDIO=0 diagnostic opt-out)")
endif()
if(DEFINED ENV{TOUCHPAD})
    add_compile_definitions(DS4_FEATURE_TOUCHPAD=1)
    target_compile_definitions(app PRIVATE DS4_FEATURE_TOUCHPAD=1)
    message(STATUS "Feature TOUCHPAD: enabled (stub, no behaviour yet)")
endif()
if(DEFINED ENV{GYRO})
    add_compile_definitions(DS4_FEATURE_GYRO=1)
    target_compile_definitions(app PRIVATE DS4_FEATURE_GYRO=1)
    message(STATUS "Feature GYRO: enabled (stub, no behaviour yet)")
endif()

# DEBUG_CONSOLE predicate: non-empty AND not "0". build_macos.sh and
# build_windows.bat use this exact same rule. A plain `if(DEFINED
# ENV{DEBUG_CONSOLE})` used to enable DEBUG_CONSOLE for
# `DEBUG_CONSOLE= ./build_macos.sh`, while the script assigned no -dbg tag and
# copied the debug firmware over the release name.
set(DS4_DEBUG_CONSOLE_ENABLED FALSE)
if(DEFINED ENV{DEBUG_CONSOLE})
    if(NOT "$ENV{DEBUG_CONSOLE}" STREQUAL "" AND
       NOT "$ENV{DEBUG_CONSOLE}" STREQUAL "0")
        set(DS4_DEBUG_CONSOLE_ENABLED TRUE)
    endif()
endif()

# Marker for the build scripts' belt-and-braces check: if this file exists,
# the firmware was configured with DEBUG_CONSOLE=1 and the script must have
# selected the -dbg output tag (and vice versa). It lives next to CMakeCache
# in the binary dir, so it is refreshed on every configure.
set(DS4_DEBUG_MARKER "${CMAKE_BINARY_DIR}/ds4_debug_console.enabled")
if(DS4_DEBUG_CONSOLE_ENABLED)
    file(WRITE "${DS4_DEBUG_MARKER}" "DEBUG_CONSOLE=1\n")
else()
    file(REMOVE "${DS4_DEBUG_MARKER}")
endif()

if(DS4_DEBUG_CONSOLE_ENABLED)
    target_sources(app PRIVATE src/debug_console.c)
    # NOTE: main.c is compiled by the <project>.elf target (created at
    # project() below), so app-target-only definitions do NOT reach it.
    # Use directory-level definitions so every target sees them.
    add_compile_definitions(DEBUG_CONSOLE=1 LOG_LEVEL=2)
    target_compile_definitions(app PRIVATE DEBUG_CONSOLE=1 LOG_LEVEL=2)
    # Logs always go to the console UART. DEBUG_CONSOLE only adds the 64 KB
    # in-RAM ring behind the optional HID feature report 0xB4 reader.
    message(STATUS "DEBUG console: UART logging + 0xB4 HID log reader")
endif()

if("$ENV{MIC_DIAG}" STREQUAL "1")
    # R17: diagnostic audio behaviour on its own switch so that a DEBUG log
    # build does not silently differ from release in what it does.
    add_compile_definitions(DS4_MIC_DIAG=1)
    target_compile_definitions(app PRIVATE DS4_MIC_DIAG=1)
    message(STATUS "Mic diagnostic mode: enabled (audio behaviour differs)")
endif()

sdk_add_include_directories(src)
sdk_add_include_directories(${BL_SDK_BASE}/components/wireless/bluetooth/blestack/src/include)
sdk_add_include_directories(${BL_SDK_BASE}/components/wireless/bluetooth/btprofile/include)
sdk_add_include_directories(${BL_SDK_BASE}/components/wireless/bluetooth/blestack/src/sbc/enc)
sdk_add_include_directories(${BL_SDK_BASE}/components/wireless/bluetooth/blestack/src/sbc/dec)
sdk_add_include_directories(${BL_SDK_BASE}/components/utils/partition)

# Board selection via env vars (mutually exclusive)
if(DEFINED ENV{BOARD_LCTECH_616})
    add_compile_definitions(BOARD_LCTECH_616=1)
    target_compile_definitions(app PRIVATE BOARD_LCTECH_616=1)
    message(STATUS "Target board: LCTech BL616")
elseif(DEFINED ENV{BOARD_M0S_DOCK})
    add_compile_definitions(BOARD_M0S_DOCK=1)
    target_compile_definitions(app PRIVATE BOARD_M0S_DOCK=1)
    message(STATUS "Target board: Sipeed M0S Dock (BL616)")
else()
    message(STATUS "Target board: Ai-M61-32S-Kit (BL618)")
endif()

# R09: Full-Speed only. FORCE_FS_MODE makes the firmware hard-code the FS
# descriptors; CONFIG_USB_HS was dropped at the top of this file so the SDK
# never enables the HS PHY. There is no HS build variant.
add_compile_definitions(FORCE_FS_MODE=1)
target_compile_definitions(app PRIVATE FORCE_FS_MODE=1)
message(STATUS "USB mode: Full-Speed only (12 Mbps, enforced; HS not supported)")

# R09 final guard: find_package() must not have reintroduced HS from a stale
# generated/defconfig.cmake. Nothing below this point may set CONFIG_USB_HS.
if(DEFINED CONFIG_USB_HS)
    message(FATAL_ERROR
        "R09: Full-Speed only — CONFIG_USB_HS was reintroduced during "
        "configuration. HS is not a supported release capability.")
endif()

project(ds4dongle_bl618)
