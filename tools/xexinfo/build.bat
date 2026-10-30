@echo off
rem Builds xexinfo.exe against the XenonUtils library produced by build-xenonrecomp.bat.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set LLVM=%LOCALAPPDATA%\Android\Sdk\ndk\27.2.12479018\toolchains\llvm\prebuilt\windows-x86_64\bin
set XR=%~dp0..\..\third_party\XenonRecomp
set B=%~dp0..\..\build\host
"%LLVM%\clang-cl.exe" /nologo /c /O2 /EHsc /MT -std:c++17 /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH /DNOMINMAX ^
  /D_CRT_SECURE_NO_WARNINGS /I"%XR%\XenonUtils" /I"%XR%\thirdparty\simde" "%~dp0xexinfo.cpp" /Fo"%B%\xexinfo.obj" || exit /b 1
"%VCToolsInstallDir%bin\Hostx64\x64\link.exe" /nologo /OUT:"%B%\xexinfo.exe" "%B%\xexinfo.obj" ^
  "%B%\XenonUtils\XenonUtils.lib" "%B%\thirdparty\disasm\disasm.lib"
"%LLVM%\clang-cl.exe" /nologo /c /O2 /EHsc /MT -std:c++17 /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH /DNOMINMAX ^
  /D_CRT_SECURE_NO_WARNINGS /I"%XR%\XenonUtils" /I"%XR%\thirdparty\simde" /I"%XR%\thirdparty\disasm" "%~dp0ppcdis.cpp" /Fo"%B%\ppcdis.obj" || exit /b 1
"%VCToolsInstallDir%bin\Hostx64\x64\link.exe" /nologo /OUT:"%B%\ppcdis.exe" "%B%\ppcdis.obj" ^
  "%B%\XenonUtils\XenonUtils.lib" "%B%\thirdparty\disasm\disasm.lib"
