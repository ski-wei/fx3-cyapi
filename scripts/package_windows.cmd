@echo off
setlocal
pushd "%~dp0.."
call "%USERPROFILE%\miniforge3\condabin\conda.bat" activate fx3-cyapi
if errorlevel 1 goto :fail
set "FX3_VSROOT="
for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "FX3_VSROOT=%%I"
if not defined FX3_VSROOT goto :fail
call "%FX3_VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 goto :fail
python -m build --wheel
if errorlevel 1 goto :fail
python -m pytest -q --junitxml=artifacts/test-results.xml
if errorlevel 1 goto :fail
popd
exit /b 0
:fail
popd
exit /b 1
