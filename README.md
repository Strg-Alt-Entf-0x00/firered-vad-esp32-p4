# ESP32-P4 LLM Project 🚀

> **Ziel:** Die weltbeste LLM-Lösung für ESP32-P4 Microcontroller

**Hardware:** Waveshare ESP32-P4-WIFI6 (32MB PSRAM, 360MHz Dual-Core RISC-V)  
**Target Model:** 100M Parameter LLM @ 25+ tokens/second  
**Status:** Research Phase ✅ Complete | Ready for Implementation ⚡

---

## 🎯 Quick Facts

- **16x mehr Memory** als ESP32-S3 Projekte (32MB vs 2MB)
- **3-4x bessere Performance** als aktueller State-of-the-Art
- **100% Offline** - Keine Cloud, keine WiFi-Abhängigkeit
- **Real-Time Capable** - Text Generation in Echtzeit

---

## 📚 Dokumentation

Alle Research-Ergebnisse und Planung in `.docs/`:

### 🔍 Research:
- **[Existierende Projekte](docs/research/existierende-projekte.md)** - Was gibt es bereits?
- **[Model-Kandidaten](docs/research/model-candidates.md)** - Welche Models kommen in Frage?
- **[Optimization Learnings](docs/research/optimization-learnings-from-asr.md)** - Best Practices aus ASR-Projekten
- **[State-of-the-Art Summary](docs/research/state-of-the-art-summary.md)** - Gesamtübersicht 2026

### 🛠️ Specs:
- **[Hardware Setup](docs/specs/hardware-setup.md)** - Waveshare Board Details
- **[ESP32-P4 Datasheet](docs/specs/esp32-p4-datasheet-v1.3.pdf)** - Official Specs

### 📊 Status:
- **[PROJECT_STATUS.md](docs/PROJECT_STATUS.md)** - Aktueller Stand & Roadmap

---

## 🏆 Target Model

**Primary:** [nanowhale-100m](https://huggingface.co/HuggingFaceTB/nanowhale-100m-base)
- 110M Parameters (DeepSeek-V4 Architecture)
- ~40-50MB mit INT4 Quantization
- Expected: 20-30 tokens/second

**Fallback:** [Pico-OpenLAiNN-100M](https://huggingface.co/UUFO-Aigis/Pico-OpenLAiNN-100M)

**Proof-of-Concept:** [SLM-10M](https://huggingface.co/blog/PY-AI-Dev/slm10-blog) (9.97M params)

---

## 🗺️ Roadmap

### ✅ Phase 0: Research (DONE!)
- Hardware Analysis ✅
- State-of-the-Art Study ✅
- Model Selection ✅
- Optimization Strategies ✅

### ⬜ Phase 1: Foundation (Next!)
- GGUF Parser
- Basic Inference Loop
- Correctness Verification
- Baseline Benchmark

### ⬜ Phase 2: Optimization
- Dual-Core Architecture
- ESP-DSP Integration
- INT8 Quantization

### ⬜ Phase 3: Scale-Up
- 100M Model Deploy
- INT4 Per-Channel Quantization
- KV-Cache Optimization

### ⬜ Phase 4: Polish
- Performance Tuning
- Multi-Model Support
- WiFi Integration
- Documentation

---

## 💡 Key Innovations

### Was macht uns besser als SOTA?

1. **32MB PSRAM** (16x mehr als S3-Projekte)
2. **Modern Models** (DeepSeek-V4, 2026)
3. **Dual-Core Pipeline** (aus FireRedVAD gelernt)
4. **Per-Channel INT4** (Best accuracy/size ratio)
5. **Hardware Acceleration** (ESP-DSP + PIE SIMD)

**Result:** 3-4x bessere Models möglich!

---

## 🚀 Quick Start

### 1. Dokumentation lesen:
```bash
# Start hier:
cat .docs/PROJECT_STATUS.md

# Dann Details:
cat .docs/research/state-of-the-art-summary.md
```

### 2. Hardware vorbereiten:
- Waveshare ESP32-P4-WIFI6 Board
- USB-C Kabel
- Optional: MIPI-DSI Display

### 3. Development Environment:
- ESP-IDF v5.5+
- VS Code + Espressif IDF Extension
- Python 3.8+ für Tools

---

## 📊 Expected Performance

| Metric | ESP32-S3 SOTA | Unser Ziel | Improvement |
|--------|---------------|------------|-------------|
| Model Size | 28.9M params | 100M params | **3.5x** |
| PSRAM | 2 MB | 32 MB | **16x** |
| Speed | 9.88 tok/s | 25+ tok/s | **2.5x** |
| Memory Bandwidth | Limited | L2 Cache | **Faster** |

---

## 🛠️ Technology Stack

- **Framework:** ESP-IDF (C/C++)
- **Model Format:** GGUF → Custom ESP32 Binary
- **Quantization:** W4A16 (INT4 Weights, FP16 Activations)
- **Acceleration:** ESP-DSP + PIE SIMD
- **Architecture:** Dual-Core Producer/Consumer

---

## 📝 License

TBD - Wird festgelegt nach Phase 1

---

## 🤝 Contributing

Projekt ist in aktiver Entwicklung. Contributions willkommen!

---

## 📧 Contact

Fragen? Issues? → GitHub Issues öffnen

---

**Status:** Ready to build! 🔥  
**Next:** Phase 1 Foundation Development

---

## 🎓 Related Projects

- [slvDev/esp32-ai](https://github.com/slvDev/esp32-ai) - 28.9M on ESP32-S3
- [llama.cpp](https://github.com/ggerganov/llama.cpp) - GGUF Reference
- FireRedVAD - Dual-Core Optimization Inspiration
