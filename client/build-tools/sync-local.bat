@echo off
REM Local override — sets correct WSL user + project path before syncing.
set "WSL_DISTRO=Debian"
set "WSL_USER=doolB"
set "WSL_PARENT=home\doolB\realmengine"
call "%~dp0sync-and-build.bat"
