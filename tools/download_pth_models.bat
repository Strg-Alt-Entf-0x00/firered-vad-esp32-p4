@echo off
echo =======================================================
echo   FireRedVAD ESP32-P4 - PyTorch Model Downloader
echo =======================================================
echo.
echo Downloading raw .pth.tar models from HuggingFace...
echo.

cd /d "%~dp0"
python download_pth_models.py --all

echo.
pause
