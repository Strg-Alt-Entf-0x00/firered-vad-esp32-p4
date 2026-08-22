@echo off
echo =======================================================
echo   FireRedVAD ESP32-P4 - Model Converter
echo =======================================================
echo.
echo Converting all PyTorch models to .frvd formats...
echo.

cd /d "%~dp0"

echo [1/4] Generating FP32 models...
python converter\export_weights.py --all --output-dir ..\examples\console_vad\frvd_models

echo.
echo [2/4] Generating INT16 models...
python converter\export_weights.py --all --output-dir ..\examples\console_vad\frvd_models --quantize-int16

echo.
echo [3/4] Generating INT8 models...
python converter\export_weights.py --all --output-dir ..\examples\console_vad\frvd_models --quantize-int8

echo.
echo [4/4] Generating INT8-CH models...
python converter\export_weights.py --all --output-dir ..\examples\console_vad\frvd_models --quantize-int8-per-ch

echo.
echo =======================================================
echo Done! Models are saved in examples\console_vad\frvd_models
echo =======================================================
pause
