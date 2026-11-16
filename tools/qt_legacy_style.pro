TEMPLATE = lib
CONFIG += plugin release
CONFIG -= debug debug_and_release
QT += widgets

isEmpty(STYLE_ROOT): error("Set STYLE_ROOT to qtstyleplugins/src/plugins/styles")
!contains(STYLE_NAME, ^(cleanlooks|motif|plastique)$): error("Choose cleanlooks, motif or plastique")
TARGET = q$${STYLE_NAME}style
INCLUDEPATH += $$STYLE_ROOT/$$STYLE_NAME
SOURCES += $$files($$STYLE_ROOT/$$STYLE_NAME/*.cpp)
HEADERS += $$files($$STYLE_ROOT/$$STYLE_NAME/*.h)
equals(STYLE_NAME, plastique): QT += core-private gui-private
!equals(STYLE_NAME, motif): include($$STYLE_ROOT/shared/shared.pri)
DESTDIR = $$OUT_PWD
