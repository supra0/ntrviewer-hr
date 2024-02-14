CC := gcc
CXX := g++
CPPFLAGS := -Iinclude
CFLAGS := -Og -g
# CFLAGS := -Ofast

ifeq ($(OS),Windows_NT)
	LDLIBS := -Llib -lmingw32 -lSDL2main -lSDL2 -lvfw32 -Wl,-Bstatic -lws2_32 -lncnn -lole32 -fopenmp -liphlpapi -static-libgcc
	# LDLIBS := -static -Llib -lmingw32 -mwindows -lSDL2main -lSDL2 -lm -lkernel32 -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -lshell32 -ldinput8
	# LDLIBS += -lvfw32 -lws2_32 -lncnn -fopenmp
	LDLIBS += -lOSDependent -lglslang -lMachineIndependent -lGenericCodeGen -lglslang-default-resource-limits -lSPIRV -lSPIRV-Tools-opt -lSPIRV-Tools -lSPIRV-Tools-link

	TARGET := ntrviewer.exe
else
	LDLIBS := -lSDL2 -lncnn

	TARGET := ntrviewer
endif
LDFLAGS :=

RM := rm

JT_SRC := $(wildcard jpeg_turbo/*.c) # jpeg_turbo/simd/x86_64/jsimd.c
JT_OBJ := $(JT_SRC:.c=.o)

# JT_SRC_S := $(wildcard jpeg_turbo/simd/x86_64/*.asm)
# JT_OBJ_S := $(JT_SRC_S:.asm=.o)

$(TARGET): main.o realcugan.o realcugan_lib.o libNK.o libNKSDL.o libGLAD.o $(JT_OBJ) $(JT_OBJ_S)
	$(CXX) $^ -o $@ $(CFLAGS) $(LDLIBS) $(LDFLAGS)

%.o: %.c
	$(CC) $^ -o $@ -c $(CFLAGS) $(CPPFLAGS)

%.o: %.asm
	nasm $^ -o $@ -DWIN64 -D__x86_64__ -fwin64 -Ijpeg_turbo/simd/nasm -Ijpeg_turbo/simd/x86_64

%.o: %.cpp
	$(CXX) $^ -o $@ -c $(CFLAGS) $(CPPFLAGS)

realcugan_lib.o: realcugan-ncnn-vulkan/lib.cpp $(wildcard srmd-realcugan-vulkan/*.h)
	$(CXX) realcugan-ncnn-vulkan/lib.cpp -o $@ -c $(CFLAGS) $(CPPFLAGS) -I../ncnn-src/src -I../ncnn-build/src -Wno-attributes

realcugan.o: realcugan-ncnn-vulkan/realcugan.cpp $(wildcard realcugan-ncnn-vulkan/*.h)
	$(CXX) realcugan-ncnn-vulkan/realcugan.cpp -o $@ -c $(CFLAGS) $(CPPFLAGS) -I../ncnn-src/src -I../ncnn-build/src -Wno-attributes

main.o: main.c
	$(CC) $^ -o $@ -c $(CFLAGS) $(CPPFLAGS) -Wall -Wextra

libNK.o: libNK.c
	$(CC) $^ -o $@ -c $(CFLAGS) $(CPPFLAGS) -std=c89 -Wall -Wextra

clean:
	$(RM) $(TARGET) *.o jpeg_turbo/*.o jpeg_turbo/simd/x86_64/*.o
