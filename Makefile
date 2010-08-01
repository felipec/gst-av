CC := $(CROSS_COMPILE)gcc

CFLAGS := -O2 -ggdb -Wall -Wextra -Wno-unused-parameter -ansi -std=c99
LDFLAGS := -Wl,--no-undefined

override CFLAGS += -D_GNU_SOURCE

GST_CFLAGS := $(shell pkg-config --cflags gstreamer-0.10 gstreamer-tag-0.10)
GST_LIBS := $(shell pkg-config --libs gstreamer-0.10 gstreamer-tag-0.10)

AVCODEC_CFLAGS := $(shell pkg-config --cflags libavcodec)
AVCODEC_LIBS := $(shell pkg-config --libs libavcodec)

all:

version := $(shell ./get-version.sh)

# plugin

gst_plugin := libgstav.so

$(gst_plugin): plugin.o gstavdec.o
$(gst_plugin): override CFLAGS += $(GST_CFLAGS) $(AVCODEC_CFLAGS) -D VERSION='"$(version)"'
$(gst_plugin): override LIBS += $(GST_LIBS) $(AVCODEC_LIBS)

targets += $(gst_plugin)

all: $(targets)

# pretty print
ifndef V
QUIET_CC    = @echo '   CC         '$@;
QUIET_LINK  = @echo '   LINK       '$@;
QUIET_CLEAN = @echo '   CLEAN      '$@;
endif

%.o:: %.c
	$(QUIET_CC)$(CC) $(CFLAGS) -MMD -o $@ -c $<

%.so::
	$(QUIET_LINK)$(CC) $(LDFLAGS) -shared -o $@ $^ $(LIBS)

clean:
	$(QUIET_CLEAN)$(RM) -v $(targets) *.o *.d

-include *.d
