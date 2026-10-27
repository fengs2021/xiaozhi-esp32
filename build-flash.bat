@echo off
chcp 65001 >nul
setlocal

rem ============================================================
rem  小智固件：编译 + 烧写（light sleep 省电版）
rem  直接双击运行。构建日志会写到 build-flash.log
rem
rem  如果编译报 IRAM 溢出（region 'iram0_0_seg' overflowed），
rem  把 main/boards/nologo/xingzhi-cube-1.54tft-wifi/config.json 里
rem  的 CONFIG_ESP_WIFI_SLP_IRAM_OPT=y 那一行删掉再试。
rem ============================================================

set "IDF_PATH=D:\esp\esp-idf"
set "IDF_TOOLS_PATH=D:\esp\espressif"
set "BOARD=nologo/xingzhi-cube-1.54tft-wifi"
set "VARIANT=xingzhi-cube-1.54tft-wifi"
set "COM_PORT=COM3"
set "BAUD=460800"

cd /d "%~dp0"
set "LOG=%CD%\build-flash.log"

echo [1/3] 加载 ESP-IDF 环境 ...
call "%IDF_PATH%\export.bat" >nul 2>&1
if errorlevel 1 (
    echo     失败：找不到 %IDF_PATH%\export.bat
    goto fail
)
echo     ok

echo [2/3] 编译固件，大约 5-10 分钟 ...
python scripts\build.py %BOARD% --name %VARIANT% --language zh-CN --wake-word wn9_xiaoaitongxue > "%LOG%" 2>&1
if errorlevel 1 (
    echo     编译失败，日志最后 30 行：
    powershell -NoProfile -Command "Get-Content '%LOG%' -Tail 30"
    goto fail
)
echo     编译成功：build\merged-binary.bin

echo [3/3] 烧写到 %COM_PORT% ...
python -m esptool --chip esp32s3 -p %COM_PORT% -b %BAUD% --before default_reset --after hard_reset write_flash 0x0 build\merged-binary.bin
if errorlevel 1 (
    echo     烧写失败。检查 USB 线是否插好、串口号是不是 %COM_PORT%
    goto fail
)

echo.
echo 全部完成。设备会自动重启。
echo 唤醒词仍然是「小爱同学」。10 分钟没人说话后屏幕和麦克风会关掉，
echo 这时候要按一下顶部的按键才能唤醒（唤醒词在省电状态下是关的）。
goto end

:fail
echo.
echo 出错了，看上面的提示。
:end
pause
