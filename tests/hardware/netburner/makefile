# QuickEIP hardware test firmware for NetBurner. See ../README.md.
# QUICKEIP is the QuickEIP checkout. A ../ path puts the objects of those
# sources outside obj/, so build from a copy with QuickEIP in a subfolder:
#   make QUICKEIP=QuickEIP PLATFORM=MODRT1171 DEFPLATFORM=MODRT1171
NAME     = QuickEIPHwTest
QUICKEIP ?= ../../..

CPP_SRC += src/main.cpp

C_SRC   += $(QUICKEIP)/src/eip_codec.c \
           $(QUICKEIP)/src/eip_adapter.c \
           $(QUICKEIP)/src/eip_session.c \
           $(QUICKEIP)/src/eip_io.c \
           $(QUICKEIP)/src/eip_scan.c \
           $(QUICKEIP)/src/eip_server.c \
           $(QUICKEIP)/src/eip_adapter_default.c
CPP_SRC += $(QUICKEIP)/ports/netburner/eip_hal_netburner.cpp
NBINCLUDE += -I$(QUICKEIP)/include -I$(QUICKEIP)/src
CFLAGS  += -std=gnu99

include $(NNDK_ROOT)/make/boilerplate.mk
