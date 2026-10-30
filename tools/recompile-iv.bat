@echo off
rem Regenerates ppc_iv\ from private\gta4\default.xex (config\gta4iv.toml), as tools\recompile.bat does
rem for EFLC: in an empty directory, hand edits re-applied, only the changed files copied.
set P=%~dp0..
if exist "%P%\build\ppc_iv.new" rmdir /s /q "%P%\build\ppc_iv.new"
mkdir "%P%\build\ppc_iv.new"
"%P%\build\host\XenonRecomp\XenonRecomp.exe" "%P%\config\gta4iv.toml" "%P%\third_party\XenonRecomp\XenonUtils\ppc_context.h" > "%P%\analysis\gta4\xenonrecomp.log" 2>&1
echo XenonRecomp exit %ERRORLEVEL%
if not "%ERRORLEVEL%"=="0" exit /b 1
if not exist "%P%\build\ppc_iv.new\ppc_func_mapping.cpp" exit /b 1
node "%P%\tools\ppc_hand_edits.js" "%P%\build\ppc_iv.new" sub_827EE370
if not "%ERRORLEVEL%"=="0" exit /b 1
node "%P%\tools\sync_dir.js" "%P%\build\ppc_iv.new" "%P%\ppc_iv"
