SUBDIRS = include

noinst_LIBRARIES = libplatform.a

libplatform_a_SOURCES = \
	platform.c \
	dai.c \
	dma.c \
	clk.c \
	timer.c \
	memory.c

libplatform_a_CFLAGS = \
	$(ARCH_CFLAGS) \
	$(ARCH_INCDIR) \
	$(SOF_INCDIR) \
	$(PLATFORM_INCDIR)
