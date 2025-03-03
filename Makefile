CC := gcc
CXX := g++
CPPFLAGS := -Iinclude
# CFLAGS := -Og -g
CFLAGS := -Ofast -g -fno-strict-aliasing
# CFLAGS += -mssse3 -mavx2
CFLAGS += -flarge-source-files
EMBED_JPEG_TURBO := 1
USE_SDL_RENDERER := 0
USE_D3D11 := 1
USE_COMPOSITION_SWAPCHAIN := 1
USE_OGL_ES := 0
USE_ANGLE := 0
GL_DEBUG := 0

ifeq ($(OS),Windows_NT)
	LDLIBS := -Llib -static -lmingw32 -lSDL2main -lSDL2
	LDLIBS += -lm -lkernel32 -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -lshell32 -ldinput8
	LDLIBS += -lws2_32 -liphlpapi
ifeq ($(findstring 1,$(USE_D3D11) $(USE_COMPOSITION_SWAPCHAIN)),1)
ifeq ($(USE_D3D11),1)
	LDLIBS += -ld3dcompiler
endif
	LDLIBS += -ld3d11 -ldxgi
endif
	TARGET := ntrviewer.exe

	NASM := -DWIN64 -fwin64
else
	LDLIBS := -static-libgcc -static-libstdc++ -Llib -Wl,-Bstatic -lSDL2

	TARGET := ntrviewer

	NASM := -DELF -felf64
endif

ifeq ($(USE_SDL_RENDERER),1)
GL_OBJ :=
else
GL_OBJ := realcugan.o realcugan_lib.o libGLAD.o fsr/fsr_main.o fsr/image_utils.o libNKSDL.o
ifeq ($(OS),Windows_NT)
ifeq ($(USE_D3D11),1)
GL_OBJ := realcugan.o realcugan_lib.o
endif
GL_OBJ += ntrviewer.res
ifeq ($(USE_COMPOSITION_SWAPCHAIN),1)
GL_OBJ += libGLAD_WGL.o
endif
endif
LDLIBS += -lncnn -fopenmp
LDLIBS += -lglslang -lMachineIndependent -lOSDependent -lGenericCodeGen -lglslang-default-resource-limits -lSPIRV -lSPIRV-Tools-opt -lSPIRV-Tools
endif

# LDFLAGS := -s

RM := rm

ifeq ($(EMBED_JPEG_TURBO),1)
JT16_SRC := $(wildcard jpeg_turbo/jpeg16/*.c)
JT16_OBJ := $(JT16_SRC:.c=.o)

JT12_SRC := $(wildcard jpeg_turbo/jpeg12/*.c)
JT12_OBJ := $(JT12_SRC:.c=.o) $(subst jpeg16,jpeg12,$(JT16_OBJ))

JT8_SRC := $(wildcard jpeg_turbo/jpeg8/*.c)
JT8_OBJ := $(JT8_SRC:.c=.o) $(subst jpeg12,jpeg8,$(JT12_OBJ))

JT_SRC_S := $(wildcard jpeg_turbo/simd/x86_64/*.asm)
JT_OBJ_S := $(JT_SRC_S:.asm=.o)

JT_SRC := $(wildcard jpeg_turbo/*.c) jpeg_turbo/simd/x86_64/jsimd.c
JT_OBJ := $(JT16_OBJ) $(JT12_OBJ) $(JT8_OBJ) $(JT_SRC:.c=.o)

CPPFLAGS += -DEMBED_JPEG_TURBO
# LDLIBS += -ljpeg
else
JT_OBJ :=
JT_OBJ_S :=

LDLIBS += -lturbojpeg
endif

# LDLIBS += -Wl,-Bdynamic

ifeq ($(USE_SDL_RENDERER),1)
CPPFLAGS += -DUSE_SDL_RENDERER
else

ifeq ($(OS),Windows_NT)
ifeq ($(USE_D3D11),1)
CPPFLAGS += -DUSE_D3D11
endif

ifeq ($(USE_COMPOSITION_SWAPCHAIN),1)
CPPFLAGS += -DUSE_COMPOSITION_SWAPCHAIN
endif
endif

ifeq ($(USE_OGL_ES),1)
CPPFLAGS += -DUSE_OGL_ES

ifeq ($(USE_ANGLE),1)
CPPFLAGS += -DUSE_ANGLE
endif

endif

ifeq ($(GL_DEBUG),1)
CPPFLAGS += -DGL_DEBUG
endif

endif

FEC_SRC := $(wildcard fecal/*.cpp)
FEC_OBJ := $(FEC_SRC:.cpp=.o) fecal/gf256_ssse3.o fecal/gf256_avx2.o fecal/gf256_ssse3_avx2.o

$(TARGET): main.o rp_syn.o ikcp.o libNK.o $(GL_OBJ) $(JT_OBJ) $(JT_OBJ_S) $(FEC_OBJ)
	$(CXX) $^ -o $@ $(CFLAGS) $(LDLIBS) $(LDFLAGS)

CC_JT = $(CC) $^ -o $@ -c $(CFLAGS) $(CPPFLAGS) -Ijpeg_turbo -Ijpeg_turbo/include -Wno-stringop-overflow

%.o: %.c
	$(CC_JT) -DBMP_SUPPORTED -DGIF_SUPPORTED -DPPM_SUPPORTED

jpeg_turbo/jpeg8/%.o: jpeg_turbo/jpeg8/%.c
	$(CC_JT) -DBMP_SUPPORTED -DPPM_SUPPORTED

jpeg_turbo/jpeg8/%.o: jpeg_turbo/jpeg12/%.c
	$(CC_JT) -DBMP_SUPPORTED -DPPM_SUPPORTED

jpeg_turbo/jpeg8/%.o: jpeg_turbo/jpeg16/%.c
	$(CC_JT) -DBMP_SUPPORTED -DPPM_SUPPORTED

jpeg_turbo/jpeg12/%.o: jpeg_turbo/jpeg12/%.c
	$(CC_JT) -DBITS_IN_JSAMPLE=12 -DGIF_SUPPORTED -DPPM_SUPPORTED

jpeg_turbo/jpeg12/%.o: jpeg_turbo/jpeg16/%.c
	$(CC_JT) -DBITS_IN_JSAMPLE=12 -DGIF_SUPPORTED -DPPM_SUPPORTED

jpeg_turbo/jpeg16/%.o: jpeg_turbo/jpeg16/%.c
	$(CC_JT) -DBITS_IN_JSAMPLE=16 -DGIF_SUPPORTED -DPPM_SUPPORTED

%.o: %.asm
	nasm $^ -o $@ $(NASM) -D__x86_64__ -Ijpeg_turbo/simd/nasm -Ijpeg_turbo/simd

%.o: %.cpp
	$(CXX) $^ -o $@ -c $(CFLAGS) $(CPPFLAGS)

ntrviewer.res: win_manifest.rc win_manifest.xml
	windres --input $< --output $@ --output-format=coff

realcugan_lib.o: realcugan-ncnn-vulkan/lib.cpp $(wildcard srmd-realcugan-vulkan/*.h)
	$(CXX) realcugan-ncnn-vulkan/lib.cpp -o $@ -c $(CFLAGS) $(CPPFLAGS) -Wno-attributes

realcugan.o: realcugan-ncnn-vulkan/realcugan.cpp $(wildcard realcugan-ncnn-vulkan/*.h)
	$(CXX) realcugan-ncnn-vulkan/realcugan.cpp -o $@ -c $(CFLAGS) $(CPPFLAGS) -Wno-attributes

fecal/gf256.o: fecal/gf256.cpp
	$(CXX) $^ -o $@ -c $(CFLAGS) $(CPPFLAGS) -DGF256_TARGET_MOBILE

fecal/gf256_ssse3.o: fecal/gf256.cpp
	$(CXX) $^ -o $@ -c $(CFLAGS) $(CPPFLAGS) -mssse3 -DGF_SUFFIX=_ssse3

fecal/gf256_avx2.o: fecal/gf256.cpp
	$(CXX) $^ -o $@ -c $(CFLAGS) $(CPPFLAGS) -mavx2 -DGF_SUFFIX=_avx2

fecal/gf256_ssse3_avx2.o: fecal/gf256.cpp
	$(CXX) $^ -o $@ -c $(CFLAGS) $(CPPFLAGS) -mssse3 -mavx2 -DGF_SUFFIX=_ssse3_avx2

main.o: main.c
	$(CC) $^ -o $@ -c $(CFLAGS) $(CPPFLAGS) -Wall -Wextra

libNK.o: libNK.c
	$(CC) $^ -o $@ -c $(CFLAGS) $(CPPFLAGS) -std=c89 -Wall -Wextra

ikcp.o: ikcp.c
	$(CC) $^ -o $@ -c $(CFLAGS) $(CPPFLAGS)

clean:
	$(RM) $(TARGET) *.o jpeg_turbo/jpeg8/*.o jpeg_turbo/jpeg12/*.o jpeg_turbo/jpeg16/*.o jpeg_turbo/*.o jpeg_turbo/simd/x86_64/*.o fsr/*.o fecal/*.o
