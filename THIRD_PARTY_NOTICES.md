# Third-party software

TF Vocal Studio is built with these open-source components:

| Component | Use | Licence |
|---|---|---|
| [JUCE](https://juce.com) 9.0.5 | Audio engine, device access (WASAPI), interface | AGPLv3 (this project is therefore AGPLv3 — see LICENSE.md) |
| [RNNoise](https://github.com/xiph/rnnoise) v0.1.1 (Jean-Marc Valin / Xiph.Org) | AI noise removal | BSD-3-Clause — see `third_party/rnnoise/COPYING` |
| [Hind Siliguri](https://fonts.google.com/specimen/Hind+Siliguri) (Indian Type Foundry) | Bengali + Latin interface font | SIL Open Font License 1.1 — see `assets/fonts/OFL.txt` |

RNNoise was modified only to replace variable-length arrays with fixed-size arrays so that it compiles with Microsoft Visual C++.
