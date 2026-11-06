@echo off
setlocal
pushd "%~dp0.."
if not exist "%USERPROFILE%\miniforge3\condabin\conda.bat" (
  echo Miniforge not found under USERPROFILE. Activate fx3-cyapi manually.
  popd
  exit /b 1
)
call "%USERPROFILE%\miniforge3\condabin\conda.bat" activate fx3-cyapi
if errorlevel 1 goto :fail
where cl >nul 2>&1
if errorlevel 1 (
  call :load_msvc
  if errorlevel 1 goto :fail
)
python -m pip install ".[test]"
if errorlevel 1 goto :fail
python -m pytest -q
if errorlevel 1 goto :fail
python -m pip check
if errorlevel 1 goto :fail
popd
exit /b 0
:load_msvc
set "FX3_VSROOT="
for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "FX3_VSROOT=%%I"
if not defined FX3_VSROOT (
  echo MSVC not found. Run from an x64 Developer Command Prompt.
  exit /b 1
)
call "%FX3_VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
exit /b %errorlevel%
:fail
popd
exit /b 1
