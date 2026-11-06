# Third-party software

TF Vocal Studio is built with these open-source components:

| Component | Use | Licence |
|---|---|---|
| [JUCE](https://juce.com) 9.0.5 | Audio engine, device access (WASAPI), interface | AGPLv3 (this project is therefore AGPLv3 — see LICENSE.md) |
| [RNNoise](https://github.com/xiph/rnnoise) v0.1.1 (Jean-Marc Valin / Xiph.Org) | AI noise removal | BSD-3-Clause — see `third_party/rnnoise/COPYING` |
| [FFmpeg](https://ffmpeg.org) 8.0 — [Gyan.dev essentials build](https://www.gyan.dev/ffmpeg/builds/) | Reading any audio/video file, showing lyrics videos | GPLv3 (separate program `ffmpeg.exe`; licence in `FFMPEG-LICENSE.txt`; source: https://ffmpeg.org/download.html) |
| [Rubber Band Library](https://breakfastquay.com/rubberband/) (commit e4296ac, Particular Programs Ltd) | Changing the music speed without changing its pitch | GPL-2.0-or-later, used here under GPLv3 terms together with AGPLv3 — see `third_party/rubberband/COPYING` |
| [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) 2.0.1 | AI music / voice separation engine | Apache-2.0 |
| [ONNX Runtime](https://github.com/microsoft/onnxruntime) | Runs the AI model on the CPU | MIT |
| UVR MDX-Net Inst HQ 3 model ([Ultimate Vocal Remover](https://github.com/Anjok07/ultimatevocalremovergui)) | AI music / voice separation model | MIT (UVR project) |
| [Hind Siliguri](https://fonts.google.com/specimen/Hind+Siliguri) (Indian Type Foundry) | Bengali + Latin interface font | SIL Open Font License 1.1 — see `assets/fonts/OFL.txt` |

RNNoise was modified only to replace variable-length arrays with fixed-size arrays so that it compiles with Microsoft Visual C++.
