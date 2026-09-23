QT       += core gui widgets network serialport

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17

TARGET = SubseaImagingSystem
TEMPLATE = app
DESTDIR = $$PWD/build
OBJECTS_DIR = $$PWD/build/obj
MOC_DIR = $$PWD/build/moc
RCC_DIR = $$PWD/build/rcc
UI_DIR = $$PWD/build/ui

# OpenCV
CONFIG += link_pkgconfig
PKGCONFIG += opencv4

# Source files
SOURCES += \
    src/main.cpp \
    src/mainwindow.cpp \
    src/configmanager.cpp \
    src/thermalcamera.cpp \
    src/visiblecamera.cpp \
    src/irlaser.cpp \
    src/heatsourcedetector.cpp \
    src/anglecalculator.cpp \
    src/tcpserver.cpp \
    src/logger.cpp

# Header files
HEADERS += \
    src/mainwindow.h \
    src/configmanager.h \
    src/thermalcamera.h \
    src/visiblecamera.h \
    src/irlaser.h \
    src/heatsourcedetector.h \
    src/anglecalculator.h \
    src/tcpserver.h \
    src/logger.h

# UI files
FORMS += \
    src/mainwindow.ui

# Resources
RESOURCES += \
    resources/resources.qrc

# Default rules
target.path = /usr/local/bin
INSTALLS += target
