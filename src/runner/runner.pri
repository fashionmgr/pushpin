QMAKE_CXXFLAGS += $$(CXXFLAGS)
QMAKE_CFLAGS += $$(CFLAGS)
QMAKE_LFLAGS += $$(LDFLAGS)

CPP_DIR = $$PWD/../cpp

INCLUDEPATH += $$CPP_DIR

DEFINES += NO_IRISNET

SOURCES += main.c
