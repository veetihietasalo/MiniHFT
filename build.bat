@echo off
echo Setting up Visual Studio Environment...
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

echo Checking for CMake...
where cmake
if %ERRORLEVEL% NEQ 0 (
    echo CMake not found even after vcvars. Exiting.
    exit /b 1
)

echo Creating build directory...
if not exist build mkdir build
cd build

echo Configuring with CMake...
cmake .. -G "NMake Makefiles"

echo Building...
cmake --build .
