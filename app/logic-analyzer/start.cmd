@echo off
setlocal
cd /d "%~dp0"
where node.exe >nul 2>nul
if errorlevel 1 (
  echo Node.js 18+ is required.
  pause
  exit /b 1
)
node.exe server.mjs %*
if errorlevel 1 pause
