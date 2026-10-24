@echo off
rem Builds the KDNET modules with the MSVC command-line tools and the WDK.
rem
rem Usage: build.cmd [arm64^|x64^|all] [test] [fault-only]   (default: all)
rem
rem Environment overrides:
rem   VSINSTALL  Visual Studio installation path (default: latest found by vswhere)
rem   WDKVER     WDK version (default: 10.0.26100.0)

setlocal EnableDelayedExpansion
set "RUN_TESTS=%~2"
set "FAULT_ONLY=%~3"
if defined FAULT_ONLY if /i not "%FAULT_ONLY%"=="fault-only" (
    echo error: third argument must be fault-only
    exit /b 1
)
if defined RUN_TESTS if /i not "%RUN_TESTS%"=="test" (
    echo error: second argument must be test
    exit /b 1
)

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
if not defined WDKVER set "WDKVER=10.0.26100.0"
set "WDKINC=%ProgramFiles(x86)%\Windows Kits\10\Include\%WDKVER%"

if not exist "%WDKINC%\km\ntddk.h" (
    echo error: WDK %WDKVER% kernel headers not found under "%WDKINC%"
    exit /b 1
)

if not defined VSINSTALL (
    for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath`) do set "VSINSTALL=%%i"
)
if not exist "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" (
    echo error: Visual Studio with ARM64 C++ tools not found. Set VSINSTALL.
    exit /b 1
)

rem Pick tools that run natively on this machine. PROCESSOR_ARCHITECTURE can
rem report AMD64 inside an emulated shell, so read the machine value instead.
set "HOST=amd64"
for /f "tokens=3" %%a in ('reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v PROCESSOR_ARCHITECTURE 2^>nul ^| findstr /i PROCESSOR_ARCHITECTURE') do (
    if /i "%%a"=="ARM64" set "HOST=arm64"
)

set "TARGETS=%~1"
if "%TARGETS%"=="" set "TARGETS=all"
if /i "%TARGETS%"=="all" set "TARGETS=arm64 x64"

for %%t in (%TARGETS%) do (
    call :build_target %%t
    if errorlevel 1 exit /b 1
)

echo.
echo Build succeeded.
exit /b 0

rem ---------------------------------------------------------------------------
:build_target
setlocal
set "TARGET=%~1"

if /i "%TARGET%"=="arm64" (
    set "VCARCH=arm64"
    if /i "%HOST%"=="amd64" set "VCARCH=amd64_arm64"
    set "MACHINE=ARM64"
    set "ARCHDEFS=/D_ARM64_ /DARM64 /D_USE_DECLSPECS_FOR_SAL=1 /DSTD_CALL"
) else if /i "%TARGET%"=="x64" (
    set "VCARCH=amd64"
    if /i "%HOST%"=="arm64" set "VCARCH=arm64_amd64"
    set "MACHINE=X64"
    set "ARCHDEFS=/D_AMD64_ /DAMD64"
) else (
    echo error: unknown target "%TARGET%"
    exit /b 1
)

echo === %TARGET% (tools: %VCARCH%)
rem vcvarsall looks for vswhere on PATH and prints an error if it is missing.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" %VCARCH% >nul
if errorlevel 1 (
    echo error: vcvarsall %VCARCH% failed
    exit /b 1
)

rem /kernel      kernel-mode code generation rules
rem /GS          security cookie checks, as the inbox modules use
rem /guard:cf    control-flow guard, as the inbox modules use
rem /Gs1048576   no stack probe calls (__chkstk)
rem /Zl          no default library references in the objects
set CFLAGS=/nologo /c /kernel /GS /guard:cf /Gs1048576 /Zl /Gy /Gw /O2 /Oi /W4 /WX /Zi ^
    /D_WIN64 /DWINNT=1 %ARCHDEFS% ^
    /I"%ROOT%\third_party\reactos" /I"%ROOT%\src\common" ^
    /I"%WDKINC%\km" /I"%WDKINC%\km\crt" /I"%WDKINC%\shared"

rem These settings make the image match the inbox kd_02_*.dll modules:
rem   /DLL /NOENTRY /SUBSYSTEM:NATIVE   kernel DLL with no entry point
rem   /DRIVER                           non-pageable sections (KDNET code must never be paged out)
rem   /RELEASE                          image checksum, which boot-loaded images need
rem   /guard:cf + bufferoverflowfastfailk.lib + libcntpr.lib
rem                                     load configuration directory, security cookie and
rem                                     CFG support, plus the security-cookie helpers ARM64
rem                                     code calls, all without imports (checked below)
rem The load configuration also gives the image a base relocation table, which the
rem boot loader needs to place the module at an address of its choosing.
set "KMLIB=%ProgramFiles(x86)%\Windows Kits\10\Lib\%WDKVER%\km\%TARGET%"
set LFLAGS=/nologo /DLL /NOENTRY /DRIVER /NODEFAULTLIB /SUBSYSTEM:NATIVE,10.00 /RELEASE ^
    /guard:cf /DEBUG /OPT:REF /OPT:ICF /INCREMENTAL:NO /DYNAMICBASE /NXCOMPAT /MACHINE:%MACHINE% ^
    /LIBPATH:"%KMLIB%" bufferoverflowfastfailk.lib libcntpr.lib

if /i "%FAULT_ONLY%"=="fault-only" goto build_fault_only
rem Probe variants (see PROBE_MODE in src\probe\probe.c). The options must be
rem quoted: batch splits unquoted arguments at "=".
call :build_module probe kd_02_15ad probe              "/DPROBE_MODE=0" || exit /b 1
call :build_module probe kd_02_15ad probe-hang-entry   "/DPROBE_MODE=1" || exit /b 1
call :build_module probe kd_02_15ad probe-hang-checked "/DPROBE_MODE=2" || exit /b 1
rem Initialization-only bring-up. Optional checkpoints use separate folders.
call :build_module vmxnet3 kd_02_15ad init-only "/DVMX_INIT_STOP_AFTER=0" || exit /b 1
call :build_module vmxnet3 kd_02_15ad transport "/DVMX_TRANSPORT=1" || exit /b 1
if defined VMX_INIT_STOP_AFTER (
    call :build_module vmxnet3 kd_02_15ad init-stop-!VMX_INIT_STOP_AFTER! "/DVMX_INIT_STOP_AFTER=!VMX_INIT_STOP_AFTER!" || exit /b 1
)
goto modules_done
:build_fault_only
call :build_module vmxnet3 kd_02_15ad fault-once "/DVMX_TRANSPORT=1 /DVMX_FAULT_INJECT=1" || exit /b 1
:modules_done
if /i "%RUN_TESTS%"=="test" (
    call :test_target || exit /b 1
)
endlocal & exit /b 0

:test_target
setlocal
set "TESTOUT=%ROOT%\out\%TARGET%\tests"
if not exist "%TESTOUT%" mkdir "%TESTOUT%"
set "EXECUTE_TESTS=1"
if /i "%TARGET%"=="arm64" if /i "%HOST%"=="amd64" set "EXECUTE_TESTS=0"
rem Build against the same WDK types, with simulated imports in user mode.
for %%s in (0 6 8 9 10 12) do (
    cl /nologo /W4 /WX /O2 /Oi /GS- /D_WIN64 /DWINNT=1 /DKDX_USE_HOST_CRT %ARCHDEFS% /DVMX_INIT_STOP_AFTER=%%s ^
        /I"%ROOT%\third_party\reactos" /I"%ROOT%\src\common" ^
        /I"%WDKINC%\km" /I"%WDKINC%\shared" ^
        /Fo"%TESTOUT%\\" /Fe"%TESTOUT%\init-test-%%s.exe" ^
        "%ROOT%\tests\vmxnet3_init_test.c" "%ROOT%\src\common\kdnet_rt.c"
    if errorlevel 1 exit /b 1
    rem ARM64 executables cannot run on an x64 host; compilation still checks them.
    if "!EXECUTE_TESTS!"=="0" (
        echo     ARM64 test execution skipped on x64 host.
    ) else (
        "%TESTOUT%\init-test-%%s.exe" || exit /b 1
    )
)
cl /nologo /W4 /WX /O2 /Oi /GS- /D_WIN64 /DWINNT=1 /DKDX_USE_HOST_CRT %ARCHDEFS% ^
    /I"%ROOT%\third_party\reactos" /I"%ROOT%\src\common" ^
    /I"%WDKINC%\km" /I"%WDKINC%\shared" ^
    /Fo"%TESTOUT%\\" /Fe"%TESTOUT%\packet-test.exe" ^
    "%ROOT%\tests\vmxnet3_packet_test.c" "%ROOT%\src\common\kdnet_rt.c"
if errorlevel 1 exit /b 1
if "%EXECUTE_TESTS%"=="1" (
    "%TESTOUT%\packet-test.exe" || exit /b 1
)
cl /nologo /W4 /WX /O2 /Oi /GS- /D_WIN64 /DWINNT=1 /DKDX_USE_HOST_CRT %ARCHDEFS% /DVMX_FAULT_INJECT=1 ^
    /I"%ROOT%\third_party\reactos" /I"%ROOT%\src\common" ^
    /I"%WDKINC%\km" /I"%WDKINC%\shared" ^
    /Fo"%TESTOUT%\\" /Fe"%TESTOUT%\fault-test.exe" ^
    "%ROOT%\tests\vmxnet3_packet_test.c" "%ROOT%\src\common\kdnet_rt.c"
if errorlevel 1 exit /b 1
if "%EXECUTE_TESTS%"=="1" (
    "%TESTOUT%\fault-test.exe" || exit /b 1
)
endlocal & exit /b 0

rem ---------------------------------------------------------------------------
rem build_module <name under src\> <output dll name> <output folder> [extra cl options]
:build_module
setlocal
set "NAME=%~1"
set "DLL=%~2"
set "VARIANT=%~3"
set "EXTRA=%~4"
set "OBJ=%ROOT%\out\obj\%TARGET%\%VARIANT%"
set "BIN=%ROOT%\out\%TARGET%\%VARIANT%"
if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%BIN%" mkdir "%BIN%"

cl %CFLAGS% %EXTRA% /Fo"%OBJ%\\" /Fd"%OBJ%\vc.pdb" "%ROOT%\src\common\kdnet_rt.c" "%ROOT%\src\%NAME%\%NAME%.c"
if errorlevel 1 exit /b 1

rem Version resource, if the module has one.
set "RES="
if exist "%ROOT%\src\%NAME%\%NAME%.rc" (
    rc /nologo %EXTRA% /fo"%OBJ%\%NAME%.res" "%ROOT%\src\%NAME%\%NAME%.rc"
    if errorlevel 1 exit /b 1
    set "RES=%OBJ%\%NAME%.res"
)

link %LFLAGS% /DEF:"%ROOT%\src\%NAME%\%NAME%.def" /OUT:"%BIN%\%DLL%.dll" /PDB:"%BIN%\%DLL%.pdb" ^
    "%OBJ%\kdnet_rt.obj" "%OBJ%\%NAME%.obj" !RES!
if errorlevel 1 exit /b 1

rem The boot loader must be able to relocate the image.
dumpbin /nologo /headers "%BIN%\%DLL%.dll" | findstr /r /c:"\] RVA \[size\] of Base Relocation Directory" | findstr /r /c:"^ *0 \[ *0\]" >nul
if not errorlevel 1 (
    echo error: %DLL%.dll has no base relocation table
    exit /b 1
)

rem KDNET rejects modules with imports or extra exports, so check both.
dumpbin /nologo /imports "%BIN%\%DLL%.dll" | findstr /c:"following imports" >nul
if not errorlevel 1 (
    echo error: %DLL%.dll has imports
    dumpbin /nologo /imports "%BIN%\%DLL%.dll"
    exit /b 1
)

dumpbin /nologo /exports "%BIN%\%DLL%.dll" > "%OBJ%\exports.txt"
findstr /c:" 1 number of functions" "%OBJ%\exports.txt" >nul && findstr /c:" KdInitializeLibrary" "%OBJ%\exports.txt" >nul
if errorlevel 1 (
    echo error: %DLL%.dll must export only KdInitializeLibrary
    type "%OBJ%\exports.txt"
    exit /b 1
)

echo     %BIN%\%DLL%.dll
endlocal & exit /b 0
