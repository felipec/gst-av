CC := $(CROSS_COMPILE)gcc

CFLAGS := -O2 -ggdb -Wall -Wextra -Wno-unused-parameter -Wmissing-prototypes -ansi
LDFLAGS := -Wl,--no-undefined -Wl,--as-needed

override CFLAGS += -std=c99 -DGST_DISABLE_DEPRECATED

GST_CFLAGS := $(shell pkg-config --cflags gstreamer-0.10 gstreamer-tag-0.10)
GST_LIBS := $(shell pkg-config --libs gstreamer-0.10 gstreamer-tag-0.10)

AVCODEC_CFLAGS := $(shell pkg-config --cflags libavcodec libavutil)
AVCODEC_LIBS := $(shell pkg-config --libs libavcodec libavutil)
AVCODEC_LIBDIR := $(shell pkg-config --variable=libdir libavcodec)

all:

version := $(shell ./get-version)
prefix := /usr

D = $(DESTDIR)

# plugin

gst_plugin := libgstav.so

$(gst_plugin): plugin.o gstav_adec.o gstav_vdec.o gstav_venc.o \
	gstav_h263enc.o gstav_h264enc.o gstav_parse.o
$(gst_plugin): override CFLAGS += -fPIC $(GST_CFLAGS) $(AVCODEC_CFLAGS) -D VERSION='"$(version)"'
$(gst_plugin): override LIBS += $(GST_LIBS) $(AVCODEC_LIBS) -Wl,--enable-new-dtags -Wl,-rpath,$(AVCODEC_LIBDIR)

targets += $(gst_plugin)

all: $(targets)

# pretty print
ifndef V
QUIET_CC    = @echo '   CC         '$@;
QUIET_LINK  = @echo '   LINK       '$@;
QUIET_CLEAN = @echo '   CLEAN      '$@;
endif

install: $(targets)
	install -m 755 -D libgstav.so $(D)$(prefix)/lib/gstreamer-0.10/libgstav.so

%.o:: %.c
	$(QUIET_CC)$(CC) $(CFLAGS) -MMD -o $@ -c $<

%.so::
	$(QUIET_LINK)$(CC) $(LDFLAGS) -shared -o $@ $^ $(LIBS)

clean:
	$(QUIET_CLEAN)$(RM) -v $(targets) *.o *.d

dist: base := gst-av-$(version)
dist:
	git archive --format=tar --prefix=$(base)/ HEAD > /tmp/$(base).tar
	mkdir -p $(base)
	echo $(version) > $(base)/.version
	chmod 664 $(base)/.version
	tar --append -f /tmp/$(base).tar --owner root --group root $(base)/.version
	rm -r $(base)
	gzip /tmp/$(base).tar

-include *.d
