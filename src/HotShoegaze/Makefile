# Project Name
TARGET = hot_shoegaze

APP_TYPE = BOOT_QSPI

OPT = -Os
CFLAGS   += -flto
CPPFLAGS += -flto
LDFLAGS  += -flto

# Library Locations
LIBDAISY_DIR = ../../libDaisy
DAISYSP_DIR  = ../../DaisySP
HOTHOUSE_DIR = ..

# Hothouse hardware abstraction
C_INCLUDES += -I$(HOTHOUSE_DIR)
CPP_SOURCES = hot_shoegaze.cpp
CPP_SOURCES += $(HOTHOUSE_DIR)/hothouse.cpp

# Core location and generic Makefile
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
include ../Makefile
