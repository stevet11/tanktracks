# Linux:   make
# Windows: (MSYS2/mingw)  make OS=Windows_NT   -- or use CMake, see README.md
RAY = third_party/raylib-5.5/src
CFLAGS = -O2 -Wall -Wno-unused-function -std=gnu11 -I$(RAY)
ifeq ($(OS),Windows_NT)
  LIBS = $(RAY)/libraylib.a -lopengl32 -lgdi32 -lwinmm
  EXE = tanktracks.exe
else
  LIBS = $(RAY)/libraylib.a -lGL -lm -lpthread -ldl -lrt -lX11
  EXE = tanktracks
endif
$(EXE): main.c $(RAY)/libraylib.a
	$(CC) $(CFLAGS) main.c -o $@ $(LIBS)
$(RAY)/libraylib.a: | $(RAY)/raylib.h
	$(MAKE) -C $(RAY) PLATFORM=PLATFORM_DESKTOP
# raylib source is not stored in git; fetch it on first build
$(RAY)/raylib.h:
	mkdir -p third_party
	curl -L https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz | tar xz -C third_party
clean:
	rm -f $(EXE)
