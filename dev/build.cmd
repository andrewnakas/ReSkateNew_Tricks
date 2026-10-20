@echo off
rem Builds ReSkate Tricks (Release). Log: dev\build.log.  build.cmd [target]
cd /d "%~dp0.."
set LOG=%~dp0build.log
if "%DINGOSDK_VERSION%"=="" set DINGOSDK_VERSION=2.1.0
echo START %DATE% %TIME% > "%LOG%"
if not exist build\vs2022-x64\DingoSDK.sln (
  cmake --preset vs2022-x64 "-DDINGOSDK_BACKTRACE_URL=" "-DDINGOSDK_VERSION=%DINGOSDK_VERSION%" >> "%LOG%" 2>&1
  echo CONFIGURE_EXIT %ERRORLEVEL% >> "%LOG%"
)
if "%~1"=="" (
  cmake --build --preset release --parallel 3 -- /v:m /nologo >> "%LOG%" 2>&1
) else (
  cmake --build --preset release --parallel 3 --target %~1 -- /v:m /nologo >> "%LOG%" 2>&1
)
echo BUILD_EXIT %ERRORLEVEL% %TIME% >> "%LOG%"
