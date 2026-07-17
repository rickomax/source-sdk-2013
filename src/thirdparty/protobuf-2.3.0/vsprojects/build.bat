@echo off
setlocal

@rem Move to the batch file directory so we can run it from anywhere
cd %~dp0

@rem Locate Visual Studio 2026 with vswhere (the legacy VSxxxCOMNTOOLS
@rem environment variables were removed after VS 2015) and set up its tools.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSINSTALLPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALLPATH=%%i"
if not defined VSINSTALLPATH (
	echo ERROR: Could not locate a Visual Studio installation with the C++ toolset.
	exit /b 1
)
call "%VSINSTALLPATH%\Common7\Tools\VsDevCmd.bat" -no_logo
set dirsuffix=\2012

@rem Note that we no longer build separate debug libraries
for /d %%d in ( "release|Win32" "release|x64") do (
	@rem Note that this message seems to display out of order for some reason...
	echo Building %%d
	devenv protobuf_2010.sln /clean %%d
	devenv protobuf_2010.sln /build %%d /project libprotobuf-lite
	@rem Note that building libprotoc also ensures that libprotobuf builds
	devenv protobuf_2010.sln /build %%d /project libprotoc
)

p4 edit ..\..\..\lib\public%dirsuffix%\libproto*.lib
p4 edit ..\..\..\lib\public\x64%dirsuffix%\libproto*.lib

copy /y Release\libproto*.lib ..\..\..\lib\public%dirsuffix%
copy /y x64\Release\libproto*.lib ..\..\..\lib\public\x64%dirsuffix%

@echo Check in the changed libraries in src\lib if you are done.
