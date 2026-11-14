# Qt 5 classic style plugins

Cleanlooks, Motif (including CDE) and Plastique are built from Qt's
[qtstyleplugins source at 335dbece](https://github.com/qt/qtstyleplugins/tree/335dbece103e2cbf6c7cf819ab6672c2956b17b3).
The source archive in this folder includes the matching upstream source and
the qmake project used for the Windows builds. The original source notices,
LGPL 2.1 text and Qt exception are included. The plugins are separate DLLs
linked against Qt 5.15.2 MSVC x64, including that version's private QtGui
headers for Plastique.

To rebuild, extract the source and use the Qt 5.15.2 MSVC x64 environment.
For each of `cleanlooks`, `motif` and `plastique`, run qmake against
`qt_legacy_style.pro` in a separate writable build folder. Set
`STYLE_ROOT` to the extracted `src/plugins/styles` and `STYLE_NAME`
to the desired plugin, then run `nmake`. Place the resulting DLL in
`styles` beside ObriiSDR.exe.
