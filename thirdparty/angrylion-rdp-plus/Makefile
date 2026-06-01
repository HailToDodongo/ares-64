
.PHONY: pj64-win32 m64p-linux m64p-win rmg-linux clean format

all: pj64-win32 m64p-linux m64p-win rmg-linux

clean:
	$(RM) -r build

format:
	$(MAKE) -f Makefile.pj64_win32 format
	$(MAKE) -f Makefile.m64p_linux format
	$(MAKE) -f Makefile.m64p_win format
	$(MAKE) -f Makefile.rmg_linux format

pj64-win32:
	$(MAKE) -f Makefile.pj64_win32

m64p-linux:
	$(MAKE) -f Makefile.m64p_linux

m64p-win:
	$(MAKE) -f Makefile.m64p_win

rmg-linux:
	$(MAKE) -f Makefile.rmg_linux
