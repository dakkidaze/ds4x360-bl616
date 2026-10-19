@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

rem ============================================================
rem  DS5Dongle BL618 - Windows build script
rem
rem  Usage: build_windows.bat [build|rebuild|clean|flash [COMx]]
rem    build   - incremental build (default, USB_SPEED=fs)
rem    rebuild - clean + full build
rem    clean   - remove build directory
rem    flash   - flash via serial, e.g. build_windows.bat flash COM5
rem
rem  Environment overrides:
rem    BL_SDK_BASE     path to the DS5Dongle BL618 SDK fork
rem    TOOLCHAIN_PATH  path to the T-Head Windows toolchain
rem    BOARD_TYPE      lctech616 (default) | aim61 | m0sdock
rem    USB_SPEED       fs (default). Only fs is supported; anything else is
rem                    rejected (Full-Speed only, 12 Mbps).
rem    AUDIO=0         diagnostic opt-out: build without UAC1 audio.
rem                    Audio is on by default (the default build is the
rem                    release candidate and a wired DS4 has audio).
rem    GAMEPAD=0       build without the HID gamepad.
rem    DEBUG_CONSOLE=1 build with the UART/0xB4 log mirror (-dbg).
rem                    Empty or 0 = off (release). Same rule as CMake.
rem    MIC_DIAG=1      build with mic diagnostics (-diag).
rem    TOUCHPAD=1 / GYRO=1  reserved stub flags.
rem
rem  Output names (firmware\%BOARD_TYPE%\):
rem    default            ds4dongle-<board>.bin        (audio + gamepad)
rem    DEBUG_CONSOLE=1    ds4dongle-<board>-dbg.bin
rem    MIC_DIAG=1         ds4dongle-<board>-diag.bin
rem    AUDIO=0            ds4dongle-<board>-noaudio.bin
rem    GAMEPAD=0          ds4dongle-<board>-nogp.bin
rem    both               ds4dongle-<board>-noaudio-nogp.bin
rem ============================================================

if "%BL_SDK_BASE%"=="" set "BL_SDK_BASE=%~dp0third_party\bouffalo_sdk"
if "%TOOLCHAIN_PATH%"=="" set "TOOLCHAIN_PATH=%USERPROFILE%\Desktop\toolchain_gcc_t-head_windows"
if "%BOARD_TYPE%"=="" set "BOARD_TYPE=lctech616"
if "%USB_SPEED%"=="" set "USB_SPEED=fs"
if /i not "%USB_SPEED%"=="fs" (
    echo ERROR: USB_SPEED=%USB_SPEED% is not supported - Full-Speed only ^(12 Mbps^); HS is not supported.
    exit /b 1
)
if "%NUMBER_OF_PROCESSORS%"=="" set "NUMBER_OF_PROCESSORS=8"

rem ---- sanity checks ----
if not exist "%BL_SDK_BASE%\project.build" (
    echo [ERROR] SDK not found at "%BL_SDK_BASE%"
    echo         Set BL_SDK_BASE to the DS5Dongle BL618 SDK fork ^(see README step 1^).
    exit /b 1
)
if not exist "%TOOLCHAIN_PATH%\bin\riscv64-unknown-elf-gcc.exe" (
    echo [ERROR] RISC-V toolchain not found at "%TOOLCHAIN_PATH%"
    echo         git clone https://gitee.com/bouffalolab/toolchain_gcc_t-head_windows.git
    echo         or set TOOLCHAIN_PATH to override.
    exit /b 1
)

rem ---- PATH ----
set "PATH=%TOOLCHAIN_PATH%\bin;%BL_SDK_BASE%\tools\make;%BL_SDK_BASE%\tools\cmake\bin;%BL_SDK_BASE%\tools\ninja;%PATH%"

set "ACTION=%~1"
if "%ACTION%"=="" set "ACTION=build"

if /i "%ACTION%"=="clean" goto :clean
if /i "%ACTION%"=="flash" goto :flash

rem build / rebuild
if /i "%ACTION%"=="rebuild" (
    if exist build make clean 2>nul
)
call :build_one
if errorlevel 1 exit /b 1
goto :done

:build_one
    rem ---- board mapping ----
    set "BOARD_LCTECH_616="
    set "BOARD_M0S_DOCK="
    if /i "%BOARD_TYPE%"=="m0sdock" (
        set "BOARD_M0S_DOCK=1"
    ) else if /i "%BOARD_TYPE%"=="aim61" (
        set "BOARD_LCTECH_616="
    ) else (
        set "BOARD_TYPE=lctech616"
        set "BOARD_LCTECH_616=1"
    )
    rem ---- feature tag: audio is on unless AUDIO=0 (diagnostic) ----
    rem DEBUG_CONSOLE predicate matches CMakeLists.txt: non-empty AND not "0".
    rem `set DEBUG_CONSOLE=` must build release, not enable the debug
    rem descriptor in CMake while naming the file as release.
    set "FEATURE_TAG="
    set "DS4_DEBUG_ON=0"
    if "%AUDIO%"=="0" set "FEATURE_TAG=!FEATURE_TAG!-noaudio"
    if not "%DEBUG_CONSOLE%"=="" if not "%DEBUG_CONSOLE%"=="0" (
        set "DS4_DEBUG_ON=1"
        set "FEATURE_TAG=!FEATURE_TAG!-dbg"
    )
    if "%MIC_DIAG%"=="1" set "FEATURE_TAG=!FEATURE_TAG!-diag"
    if not "%TOUCHPAD%"=="" set "FEATURE_TAG=!FEATURE_TAG!-tp"
    if not "%GYRO%"=="" set "FEATURE_TAG=!FEATURE_TAG!-gyro"
    if "%GAMEPAD%"=="0" set "FEATURE_TAG=!FEATURE_TAG!-nogp"
    rem ---- usb speed: Full-Speed only (rejected at the top otherwise) ----
    set "BUILD_KEY=%BOARD_TYPE%-fs!FEATURE_TAG!"
    set "BOARD_STAMP=build\.board_type"
    if exist "%BOARD_STAMP%" (
        set "PREV_KEY="
        set /p PREV_KEY=<"%BOARD_STAMP%"
        if not "!PREV_KEY!"=="!BUILD_KEY!" (
            echo [build] Config changed ^(!PREV_KEY! -^> !BUILD_KEY!^), forcing clean...
            if exist build make clean 2>nul
            if exist "build\build_out\CMakeCache.txt" del /q "build\build_out\CMakeCache.txt" 2>nul
        )
    )
    set "BIN_NAME=ds4dongle-%BOARD_TYPE%!FEATURE_TAG!.bin"
    echo [build] Target: %BOARD_TYPE%  USB: %USB_SPEED%
    echo [build] Audio: default ^(AUDIO=0 to disable^)  Tag: "!FEATURE_TAG!"
    make -j%NUMBER_OF_PROCESSORS%
    if errorlevel 1 exit /b 1
    rem ---- DEBUG_CONSOLE consistency: CMake marker must match the -dbg tag ----
    rem CMakeLists.txt writes build\ds4_debug_console.enabled only when it
    rem enabled the DEBUG_CONSOLE macro. Refuse to copy a debug firmware to
    rem the release file name (or the reverse).
    if "%DS4_DEBUG_ON%"=="1" goto :dbg_expect_on
    if exist "build\ds4_debug_console.enabled" (
        echo [ERROR] CMake enabled DEBUG_CONSOLE but no -dbg tag was selected.
        echo         Refusing to overwrite the release firmware with a debug build.
        exit /b 1
    )
    goto :dbg_check_done
    :dbg_expect_on
    if not exist "build\ds4_debug_console.enabled" (
        echo [ERROR] -dbg selected but CMake did not enable DEBUG_CONSOLE.
        echo         DEBUG_CONSOLE must be non-empty and not 0 - empty or 0 means release.
        exit /b 1
    )
    :dbg_check_done
    if not exist build mkdir build
    >"%BOARD_STAMP%" echo !BUILD_KEY!
    set "OUT_DIR=firmware\%BOARD_TYPE%"
    if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
    copy /y "build\build_out\ds4dongle_bl618_bl616.bin" "%OUT_DIR%\!BIN_NAME!" >nul
    copy /y "build\build_out\boot2_bl616_isp_release_v8.1.8.bin" "%OUT_DIR%\" >nul
    copy /y "build\build_out\partition.bin" "%OUT_DIR%\" >nul
    echo [build] Output: %OUT_DIR%\!BIN_NAME!
    goto :eof

:clean
make clean
goto :done

:flash
set "COMX=%~2"
if "%COMX%"=="" set "COMX=COM5"
make flash COMX=%COMX%
goto :done

:done
echo [build] Done.
exit /b %ERRORLEVEL%
