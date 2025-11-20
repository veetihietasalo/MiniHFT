@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

echo Compiling...
cl /EHsc /std:c++20 /I include src\main.cpp /Fe:MiniHFT.exe

if %ERRORLEVEL% EQU 0 (
    echo Compilation Successful!
    echo Running MiniHFT.exe...
    MiniHFT.exe
) else (
    echo Compilation Failed.
)
