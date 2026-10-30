@echo off
rem Regenerates ppc\ from private\default.xex. XenonRecomp always writes into an empty directory
rem (it neither rewrites unchanged files nor deletes ones a previous run left over: stale files =
rem duplicate symbols); ppc_hand_edits.js re-applies the hand edits of the output and sync_dir.js
rem then copies only the files that changed into ppc\, so ninja rebuilds just those.
set P=%~dp0..
if exist "%P%\build\ppc.new" rmdir /s /q "%P%\build\ppc.new"
mkdir "%P%\build\ppc.new"
"%P%\build\host\XenonRecomp\XenonRecomp.exe" "%P%\config\gta4eflc.toml" "%P%\third_party\XenonRecomp\XenonUtils\ppc_context.h" > "%P%\analysis\xenonrecomp.log" 2>&1
echo XenonRecomp exit %ERRORLEVEL%
rem "if errorlevel 1" misses crashes (negative exit codes such as 0xC0000409).
if not "%ERRORLEVEL%"=="0" exit /b 1
if not exist "%P%\build\ppc.new\ppc_func_mapping.cpp" exit /b 1
node "%P%\tools\ppc_hand_edits.js" "%P%\build\ppc.new"
if not "%ERRORLEVEL%"=="0" exit /b 1
node "%P%\tools\sync_dir.js" "%P%\build\ppc.new" "%P%\ppc"
