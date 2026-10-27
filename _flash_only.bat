@echo off
chcp 65001 >nul
setlocal
set "IDF_PATH=D:\esp\esp-idf"
set "IDF_TOOLS_PATH=D:\esp\espressif"
cd /d "%~dp0"
call "%IDF_PATH%\export.bat" >nul 2>&1
if errorlevel 1 ( echo EXPORT_FAILED & exit /b 1 )
python -m esptool --chip esp32s3 -p COM3 -b 115200 --before default-reset --after hard-reset write-flash 0x0 build\merged-binary.bin
if errorlevel 1 ( echo FLASH_FAILED & exit /b 1 )
echo FLASH_OK
exit /b 0
