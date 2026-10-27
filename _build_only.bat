@echo off
chcp 65001 >nul
setlocal
set "IDF_PATH=D:\esp\esp-idf"
set "IDF_TOOLS_PATH=D:\esp\espressif"
cd /d "%~dp0"
set "LOG=%CD%\_build_only.log"
echo [1/2] export.bat ...
call "%IDF_PATH%\export.bat" >nul 2>&1
if errorlevel 1 ( echo EXPORT_FAILED & exit /b 1 )
echo [2/2] build.py ...
python scripts\build.py nologo/xingzhi-cube-1.54tft-wifi --name xingzhi-cube-1.54tft-wifi --language zh-CN --wake-word wn9_xiaoaitongxue > "%LOG%" 2>&1
if errorlevel 1 ( echo BUILD_FAILED & exit /b 1 )
echo BUILD_OK
exit /b 0
