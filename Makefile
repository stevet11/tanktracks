# Linux:   make
# Windows: (MSYS2/mingw)  make OS=Windows_NT   -- or use CMake, see README.md
RAY = third_party/raylib-5.5/src
ENET = third_party/enet-1.3.18
ENETSRC = callbacks compress host list packet peer protocol
CFLAGS = -O2 -Wall -Wno-unused-function -std=gnu11 -I$(RAY) -I$(ENET)/include
ENETFLAGS = -O2 -I$(ENET)/include
ifeq ($(OS),Windows_NT)
  ENETSRC += win32
  LIBS = $(RAY)/libraylib.a -lopengl32 -lgdi32 -lwinmm -lws2_32
  EXE = tanktracks.exe
else
  ENETSRC += unix
  ENETFLAGS += -DHAS_FCNTL=1 -DHAS_POLL=1 -DHAS_GETADDRINFO=1 -DHAS_GETNAMEINFO=1 -DHAS_GETHOSTBYNAME_R=1 -DHAS_GETHOSTBYADDR_R=1 -DHAS_INET_PTON=1 -DHAS_INET_NTOP=1 -DHAS_MSGHDR_FLAGS=1 -DHAS_SOCKLEN_T=1
  LIBS = $(RAY)/libraylib.a -lGL -lm -lpthread -ldl -lrt -lX11
  EXE = tanktracks
endif
ENETOBJ = $(addprefix $(ENET)/,$(addsuffix .o,$(ENETSRC)))
$(EXE): main.c net.c net.h $(RAY)/libraylib.a $(ENETOBJ)
	$(CC) $(CFLAGS) main.c net.c $(ENETOBJ) -o $@ $(LIBS)
$(ENET)/%.o: $(ENET)/%.c | $(ENET)/include/enet/enet.h
	$(CC) $(ENETFLAGS) -c $< -o $@
$(RAY)/libraylib.a: | $(RAY)/raylib.h
	$(MAKE) -C $(RAY) PLATFORM=PLATFORM_DESKTOP
# raylib and ENet sources are not stored in git; fetch them on first build
$(RAY)/raylib.h:
	mkdir -p third_party
	curl -L https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz | tar xz -C third_party
$(ENET)/include/enet/enet.h:
	mkdir -p third_party
	curl -L https://github.com/lsalzman/enet/archive/refs/tags/v1.3.18.tar.gz | tar xz -C third_party
clean:
	rm -f $(EXE) tanktracks.exe $(ENET)/*.o

# Cross-compile a Windows .exe from Linux (needs mingw-w64: sudo apt install mingw-w64). Builds in build-win/ so the
# Linux objects are untouched.  make windows  ->  tanktracks.exe
WCC = x86_64-w64-mingw32-gcc
WAR = x86_64-w64-mingw32-ar
WENET = callbacks compress host list packet peer protocol win32
windows: | $(RAY)/raylib.h $(ENET)/include/enet/enet.h
	mkdir -p build-win/raylib build-win/enet
	[ -f build-win/raylib/libraylib.a ] || { cp -r $(RAY)/. build-win/raylib/ && rm -f build-win/raylib/*.o build-win/raylib/libraylib.a && \
	  $(MAKE) -C build-win/raylib PLATFORM=PLATFORM_DESKTOP OS=Windows_NT CC=$(WCC) AR=$(WAR) RAYLIB_RELEASE_PATH=. ; }
	for f in $(WENET); do $(WCC) -O2 -I$(ENET)/include -c $(ENET)/$$f.c -o build-win/enet/$$f.o || exit 1; done
	$(WCC) -O2 -Wall -Wno-unused-function -std=gnu11 -Ibuild-win/raylib -I$(ENET)/include main.c net.c build-win/enet/*.o \
	  build-win/raylib/libraylib.a -static -lopengl32 -lgdi32 -lwinmm -lws2_32 -o tanktracks.exe
.PHONY: windows clean
