# Builds RadeonNIGA.plugin with Apple's gcc on Mac OS X. scripts/build-mac.sh
# runs it with SDK=/Developer/SDKs/MacOSX10.5.sdk.

NAME   = RadeonNIGA
BUILD  = build
BUNDLE = $(BUILD)/$(NAME).plugin
SDK    = /Developer/SDKs/MacOSX10.4u.sdk
CFLAGS = -arch ppc -Os -Wall -isysroot $(SDK) -I../hw

all: $(BUNDLE)

$(BUNDLE): $(NAME).c Info.plist ../hw/rdn_user.h
	@mkdir -p $(BUNDLE)/Contents/MacOS
	gcc $(CFLAGS) -bundle -o $(BUNDLE)/Contents/MacOS/$(NAME) $(NAME).c \
		-framework CoreFoundation -framework IOKit
	cp Info.plist $(BUNDLE)/Contents/Info.plist

clean:
	rm -rf $(BUILD)

.PHONY: all clean
