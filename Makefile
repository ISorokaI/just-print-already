CC      = clang
CFLAGS  = -Wall -O2 -arch arm64 -Idrivers/zjs
LDFLAGS = -lcups -lcupsimage
APP     = build/JustPrintAlready.app
RES     = $(APP)/Contents/Resources

all: app

build/rastertozjs: drivers/zjs/rastertozjs.c drivers/zjs/jbig.c drivers/zjs/jbig_ar.c
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

app: build/rastertozjs
	swift build -c release --package-path App
	rm -rf $(APP)
	mkdir -p $(APP)/Contents/MacOS $(RES)/filters $(RES)/zjs
	cp App/.build/release/JustPrintAlready $(APP)/Contents/MacOS/
	cp App/Info.plist $(APP)/Contents/
	cp build/rastertozjs $(RES)/filters/
	cp drivers/models.tsv drivers/testpage.pdf scripts/install-queue.sh $(RES)/
	cp drivers/zjs/template.ppd $(RES)/zjs/
	codesign --force --deep --sign - $(APP)

test:
	swift test --package-path App

dmg: app
	rm -f build/JustPrintAlready.dmg
	hdiutil create -volname "Just Print Already" -srcfolder $(APP) -ov -format UDZO build/JustPrintAlready.dmg

clean:
	rm -rf build App/.build

.PHONY: all app test dmg clean
