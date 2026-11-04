# QuickEIP NetBurner example. In your own project, put QuickEIP in a
# subfolder and set QUICKEIP = QuickEIP (a ../ path puts objects outside obj/).
NAME     = QuickEIPAdapter
QUICKEIP ?= ../..

CPP_SRC += src/main.cpp

# ---- QuickEIP (copy these lines into your own project's makefile) ----
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
# ----------------------------------------------------------------------

include $(NNDK_ROOT)/make/boilerplate.mk
