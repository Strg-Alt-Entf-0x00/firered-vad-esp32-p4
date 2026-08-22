@echo off
echo =======================================================
echo   FireRedVAD ESP32-P4 - Model Downloader
echo =======================================================
echo.
echo Downloading .frvd models from HuggingFace...
echo.

cd /d "%~dp0"
python download_frvd_models.py

echo.
pause
