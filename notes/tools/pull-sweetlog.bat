@echo off
rem Pull the iOS sweetlog from the WP210 (phone booted to Android, adb + root).
rem Reads only: copies the log area of /dev/block/sdc to /sdcard, then to this folder
rem as sweetlog.bin and as a time-stamped copy, so earlier logs are kept.
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set T=%%i
adb shell "su -c 'dd if=/dev/block/sdc of=/sdcard/sweetlog.bin bs=4096 skip=124955639 count=1025'" || goto fail
adb pull /sdcard/sweetlog.bin sweetlog.bin || goto fail
copy /y sweetlog.bin sweetlog-%T%.bin >nul
for %%f in (sweetlog.bin) do echo Saved sweetlog.bin and sweetlog-%T%.bin (%%~zf bytes, expected 4198400)
exit /b 0
:fail
echo FAILED - is the phone in Android with adb connected and root allowed?
exit /b 1
