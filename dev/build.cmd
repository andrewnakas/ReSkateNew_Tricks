@echo off
rem Builds ReSkate Tricks (Release). Log: dev\build.log.  build.cmd [target]
cd /d D:\ReSkateNew_Tricks
set LOG=D:\ReSkateNew_Tricks\dev\build.log
echo START %DATE% %TIME% > "%LOG%"
if not exist build\vs2022-x64\DingoSDK.sln (
  cmake --preset vs2022-x64 "-DDINGOSDK_BACKTRACE_URL=" "-DDINGOSDK_VERSION=2.0.7" >> "%LOG%" 2>&1
  echo CONFIGURE_EXIT %ERRORLEVEL% >> "%LOG%"
)
if "%~1"=="" (
  cmake --build --preset release --parallel 3 -- /v:m /nologo >> "%LOG%" 2>&1
) else (
  cmake --build --preset release --parallel 3 --target %~1 -- /v:m /nologo >> "%LOG%" 2>&1
)
echo BUILD_EXIT %ERRORLEVEL% %TIME% >> "%LOG%"
