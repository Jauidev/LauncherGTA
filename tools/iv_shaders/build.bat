@echo off
rem Builds iv_shader_tool.exe (see iv_shader_tool.cpp) into build\host\iv_shaders.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set LLVM=%LOCALAPPDATA%\Android\Sdk\ndk\27.2.12479018\toolchains\llvm\prebuilt\windows-x86_64\bin
set ROOT=%~dp0..\..
set X=%ROOT%\third_party\rexglue\thirdparty\XenosRecomp\XenosRecomp
set T=%ROOT%\third_party\rexglue\thirdparty
set OUT=%ROOT%\build\host\iv_shaders
if not exist "%OUT%" mkdir "%OUT%"
"%LLVM%\clang-cl.exe" /nologo /O1 /EHsc /std:c++20 /MT -w /D_CRT_SECURE_NO_WARNINGS /DNDEBUG /DFMT_HEADER_ONLY /DZSTD_DISABLE_ASM /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH ^
  /I"%X%" /I"%T%\dxc-bin\inc" /I"%T%\smol-v\source" /I"%T%\zstd\lib" /I"%T%\zstd\lib\common" ^
  /I"%ROOT%\third_party\XenonRecomp\thirdparty\fmt\include" ^
  "%~dp0iv_shader_tool.cpp" "%T%\smol-v\source\smolv.cpp" ^
  "%T%\zstd\lib\common\debug.c" "%T%\zstd\lib\common\entropy_common.c" "%T%\zstd\lib\common\error_private.c" ^
  "%T%\zstd\lib\common\fse_decompress.c" "%T%\zstd\lib\common\pool.c" "%T%\zstd\lib\common\threading.c" ^
  "%T%\zstd\lib\common\xxhash.c" "%T%\zstd\lib\common\zstd_common.c" ^
  "%T%\zstd\lib\decompress\huf_decompress.c" "%T%\zstd\lib\decompress\zstd_ddict.c" ^
  "%T%\zstd\lib\decompress\zstd_decompress.c" "%T%\zstd\lib\decompress\zstd_decompress_block.c" ^
  /c /Fo"%OUT%\\" || exit /b 1
"%VCToolsInstallDir%bin\Hostx64\x64\link.exe" /nologo /OUT:"%OUT%\iv_shader_tool.exe" "%OUT%\*.obj"
