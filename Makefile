CC := gcc
CFLAGS := -O2 -ggdb -Wall -Wextra -Wno-unused-parameter -ansi -std=c99

override CFLAGS += -D_GNU_SOURCE

GST_CFLAGS := $(shell pkg-config --cflags gstreamer-0.10)
GST_LIBS := $(shell pkg-config --libs gstreamer-0.10)

AVCODEC_CFLAGS := $(shell pkg-config --cflags libavcodec)
AVCODEC_LIBS := $(shell pkg-config --libs libavcodec)

all:

version := $(shell ./get-version.sh)

# plugin

gst_plugin := libgstav.so

$(gst_plugin): plugin.o gstavdec.o
$(gst_plugin): CFLAGS := $(CFLAGS) $(GST_CFLAGS) $(AVCODEC_CFLAGS) -D VERSION='"$(version)"'
$(gst_plugin): LIBS := $(GST_LIBS) $(AVCODEC_LIBS)

targets += $(gst_plugin)

all: $(targets)

# pretty print
V = @
Q = $(V:y=)
QUIET_CC    = $(Q:@=@echo '   CC         '$@;)
QUIET_LINK  = $(Q:@=@echo '   LINK       '$@;)
QUIET_CLEAN = $(Q:@=@echo '   CLEAN      '$@;)

%.o:: %.c
	$(QUIET_CC)$(CC) $(CFLAGS) -MMD -o $@ -c $<

%.so::
	$(QUIET_LINK)$(CC) $(LDFLAGS) -shared -Wl,--no-undefined -o $@ $^ $(LIBS)

clean:
	$(QUIET_CLEAN)$(RM) -v $(targets) *.o *.d

-include *.d
