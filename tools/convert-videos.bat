@echo off
rem Sonic Mania 3DS: the video converter's window (convert-videos.py), by a double click.
rem Finds Python 3 with tkinter and ffmpeg; offers to install what is missing with winget (Windows 10 and 11),
rem or opens its download page where there is no winget.
setlocal EnableExtensions
title Sonic Mania 3DS - video converter
set "HERE=%~dp0"

call :find_python
if defined PY goto have_python
echo Python 3 is needed to convert the videos, and it is not installed.
call :install Python.Python.3.13 "Python 3" "https://www.python.org/downloads/"
if errorlevel 1 goto end
call :find_python
if defined PY goto have_python
echo Python is installed now, but this window cannot see it yet: run convert-videos.bat again.
goto end
:have_python

call :find_ffmpeg
if defined FFMPEG goto have_ffmpeg
echo ffmpeg is needed to convert the videos, and it is not installed.
call :install Gyan.FFmpeg.Essentials "ffmpeg" "https://ffmpeg.org/download.html"
if errorlevel 1 goto end
call :find_ffmpeg
if defined FFMPEG goto have_ffmpeg
echo ffmpeg is installed now, but this window cannot see it yet: run convert-videos.bat again.
goto end
:have_ffmpeg

rem The window without a console behind it (pythonw), when there is one; this window closes
set "PYW=%PY:python.exe=pythonw.exe%"
if not exist "%PYW%" goto console_python
start "" "%PYW%" "%HERE%convert-videos.py" --gui --ffmpeg "%FFMPEG%"
exit /b 0
:console_python
echo Opening the video converter...
"%PY%" "%HERE%convert-videos.py" --gui --ffmpeg "%FFMPEG%"
if errorlevel 1 goto end
exit /b 0

:end
echo.
pause
exit /b 1

rem ---------------------------------------------------------------- Python 3 with tkinter
:find_python
set "PY="
rem The py launcher first: "python" may be the Microsoft Store's stand-in, which only opens the Store
for /f "delims=" %%P in ('py -3 -c "import sys, tkinter; print(sys.executable)" 2^>nul') do set "PY=%%P"
if defined PY exit /b 0
for /f "delims=" %%P in ('where python 2^>nul ^| findstr /v /i "WindowsApps"') do if not defined PY call :try_python "%%P"
if defined PY exit /b 0
rem Where winget puts it, before this window's PATH knows about it
for /d %%D in ("%LOCALAPPDATA%\Programs\Python\Python3*") do if not defined PY call :try_python "%%D\python.exe"
exit /b 0

:try_python
if not exist "%~1" exit /b 0
"%~1" -c "import tkinter" >nul 2>nul && set "PY=%~1"
exit /b 0

rem ---------------------------------------------------------------- ffmpeg
:find_ffmpeg
set "FFMPEG="
for /f "delims=" %%F in ('where ffmpeg 2^>nul') do if not defined FFMPEG set "FFMPEG=%%F"
if defined FFMPEG exit /b 0
if exist "%LOCALAPPDATA%\Microsoft\WinGet\Links\ffmpeg.exe" set "FFMPEG=%LOCALAPPDATA%\Microsoft\WinGet\Links\ffmpeg.exe"
exit /b 0

rem ---------------------------------------------------------------- install %1 (winget id), %2 (name), %3 (its page)
:install
where winget >nul 2>nul
if errorlevel 1 goto install_page
choice /c YN /m "Download and install %~2 now with winget"
if errorlevel 2 goto install_declined
winget install -e --id %~1 --accept-package-agreements --accept-source-agreements
if errorlevel 1 goto install_failed
exit /b 0
:install_failed
echo winget could not install %~2. Opening its download page: install it from there, then run this again.
start "" "%~3"
exit /b 1
:install_page
echo There is no winget on this Windows. Opening the download page of %~2: install it, then run this again.
start "" "%~3"
exit /b 1
:install_declined
echo Without %~2 the videos cannot be converted. Its download page: %~3
exit /b 1
