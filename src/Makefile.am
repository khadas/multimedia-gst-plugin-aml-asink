# Note: plugindir is set in configure

##############################################################################
# time pair parameter lib
##############################################################################
lib_LTLIBRARIES = libgstamltimepair.la
libgstamltimepair_la_SOURCES = gstparam_time_pair.c

libgstamltimepair_la_CFLAGS = $(GST_CFLAGS)
libgstamltimepair_la_LIBADD = $(GST_LIBS)
libgstamltimepair_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)

include_HEADERS = gstparam_time_pair.h

##############################################################################
# plugin
##############################################################################
plugin_LTLIBRARIES = libgstamlhalasink.la

# sources used to compile this plug-in
libgstamlhalasink_la_SOURCES = gstamlhalasink_new.c \
			       ac4_frame_parse.h \
			       ac4_frame_parse.c \
			       scaletempo.h \
			       scaletempo.c \
			       gstamlclock.c \
			       mediasync_wrap.c

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstamlhalasink_la_CFLAGS = $(GST_CFLAGS)
libgstamlhalasink_la_LIBADD = $(GST_LIBS)
libgstamlhalasink_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstamlhalasink_la_LIBADD += -L$(TARGET_DIR)/usr/lib -laudio_client -lamlavsync -lgstamltimepair
libgstamlhalasink_la_LIBTOOLFLAGS = --tag=disable-static

if AD
libgstamlhalasink_la_SOURCES += pes_private_data.c
libgstamlhalasink_la_LDFLAGS += -lgstpesmeta
libgstamlhalasink_la_CFLAGS += -DSUPPORT_AD
endif

if MS12
libgstamlhalasink_la_CFLAGS += -DENABLE_MS12
endif

if DTS
libgstamlhalasink_la_CFLAGS += -DENABLE_DTS
endif

if ESSOS
libgstamlhalasink_la_CFLAGS += -DESSOS_RM
libgstamlhalasink_la_LIBADD += -lessosrmgr
endif

if MEDIA_SYNC
libgstamlhalasink_la_CFLAGS += -DENABLE_MEDIA_SYNC
endif

# headers we need but don't want installed
#noinst_HEADERS =
include_HEADERS = gstamlclock.h gstamlhalasink_new.h mediasync_wrap.h

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = amlhalasink.pc

##############################################################################
# test binary #
##############################################################################
#bin_PROGRAMS = ts_pair_test

#ts_pair_test_SOURCES = param_time_pair_test.c gstparam_time_pair.c

#ts_pair_test_CFLAGS = $(GST_CFLAGS)
#ts_pair_test_LDADD = $(GST_LIBS)
