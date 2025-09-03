@echo off
REM Build Backup Script for PlasmaVulkan
REM Usage: backup_build.bat [description]

setlocal enabledelayedexpansion

REM Get the next backup number
set /a count=0
for /d %%d in (build_backups\*) do (
    set /a count+=1
)
set /a count+=1

REM Pad the number with zeros
if %count% lss 10 (
    set "num=00%count%"
) else if %count% lss 100 (
    set "num=0%count%"
) else (
    set "num=%count%"
)

REM Get description from argument or use default
if "%~1"=="" (
    set "desc=backup"
) else (
    set "desc=%~1"
)

REM Create backup directory name
set "backup_dir=build_backups\%num%_%desc%"

REM Create directory and copy files
echo Creating backup: %backup_dir%
mkdir "%backup_dir%" 2>nul

REM Copy the executable
copy "cmake-build-debug-visual-studio\Debug\PlasmaVulkan.exe" "%backup_dir%\" >nul 2>&1
if %errorlevel% neq 0 (
    copy "cmake-build-debug-visual-studio\Release\PlasmaVulkan.exe" "%backup_dir%\" >nul 2>&1
)

REM Copy shader directory
xcopy "cmake-build-debug-visual-studio\Debug\shaders" "%backup_dir%\shaders\" /E /I /Q >nul 2>&1

REM Create info file with timestamp and description
echo Build Backup #%num% > "%backup_dir%\info.txt"
echo Date: %date% %time% >> "%backup_dir%\info.txt"
echo Description: %desc% >> "%backup_dir%\info.txt"

echo Backup complete: %backup_dir%