@echo off
rem Test-signs every built kd_*.dll under out\.
rem
rem Usage: sign.cmd [basic^|windows] [all^|fault-once]   (defaults: basic all)
rem
rem See tools\sign.ps1 for what each certificate profile contains.

setlocal
set "PROFILE=%~1"
if "%PROFILE%"=="" set "PROFILE=Basic"
set "VARIANT=%~2"
if "%VARIANT%"=="" set "VARIANT=All"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\sign.ps1" -Profile %PROFILE% -Variant %VARIANT%
exit /b %errorlevel%
