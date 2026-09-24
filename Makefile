CC ?= cc
PKG_CONFIG ?= pkg-config

BUILD_DIR := build
TARGET := $(BUILD_DIR)/usb_update
SOURCES := libusb_usb_update.c libusb_vid_pid.c serial_boot.c image_package.c progress.c
OBJECTS := $(addprefix $(BUILD_DIR)/,$(SOURCES:.c=.o))
DEPS := $(OBJECTS:.o=.d)

LIBUSB_CFLAGS := $(shell $(PKG_CONFIG) --cflags libusb-1.0 2>/dev/null)
LIBUSB_LIBS := $(shell $(PKG_CONFIG) --libs libusb-1.0 2>/dev/null)

ifeq ($(strip $(LIBUSB_LIBS)),)
$(error libusb-1.0 development files not found; install libusb-1.0 and pkg-config)
endif

CPPFLAGS += $(LIBUSB_CFLAGS)
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra
LDLIBS += $(LIBUSB_LIBS)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS) | $(BUILD_DIR)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR):
	mkdir -p $@

clean:
	$(RM) -r $(BUILD_DIR)

-include $(DEPS)
