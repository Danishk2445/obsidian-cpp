JKQtPlotter subset (JKQTCommon + JKQTMathText + XITS fonts)
Upstream: https://github.com/jkriege2/JKQtPlotter
Commit: 9292d87ac78e99331f51a05bca867a92da219aa5 (2026-09-13)
License: LGPL-2.1 (LICENSE); XITS fonts: SIL OFL 1.1 (lib/jkqtmathtext/resources/xits/OFL.txt)
Trimmed: examples, tools, tests, doc, screenshots, other sub-libraries, FiraMath font, XITS sources.
Built as a shared library (LGPL).

Patches (marked "obsidian-cpp patch"):
- JKQTMathText::getFontSize() returns the size in the active unit (pixel sizes made \displaystyle, \tiny... use -1).
- JKQTMathTextEnvironment::getFont() rounds pixel sizes and keeps them >= 1.
- Symbol font scaling also works for pixel-sized fonts.
