@echo off
rem Builds XenonRecomp/XenonAnalyse for the Windows host with the NDK's clang-cl and the MSVC Build Tools.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set LLVM=%LOCALAPPDATA%\Android\Sdk\ndk\27.2.12479018\toolchains\llvm\prebuilt\windows-x86_64\bin
set CM=%LOCALAPPDATA%\Android\Sdk\cmake\3.31.6\bin
set SRC=%~dp0third_party\XenonRecomp
set OUT=%~dp0build\host
if not exist "%OUT%\build.ninja" (
  "%CM%\cmake" -S "%SRC%" -B "%OUT%" -G Ninja -DCMAKE_MAKE_PROGRAM="%CM%\ninja.exe" ^
    -DCMAKE_C_COMPILER="%LLVM%\clang-cl.exe" -DCMAKE_CXX_COMPILER="%LLVM%\clang-cl.exe" ^
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="/D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH /EHsc" -DCMAKE_C_FLAGS="/D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH" || exit /b 1
)
"%CM%\cmake" --build "%OUT%" -j 8 --target XenonAnalyse XenonRecomp
