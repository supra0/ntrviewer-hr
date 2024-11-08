CC := gcc
CXX := g++
CPPFLAGS := -Iinclude -DNDEBUG
# CFLAGS := -Og -g
CFLAGS := -Ofast -g -fno-strict-aliasing
CFLAGS += -Wall -Wextra -flarge-source-files -MMD
EMBED_JPEG_TURBO := 1

ifeq ($(OS),Windows_NT)
	LDLIBS := -Llib -static -lmingw32 -lSDL2main -lSDL2 -lm -lkernel32 -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -lshell32 -ldinput8 -lws2_32 -liphlpapi -ld3dcompiler -ld3d11 -ldxgi
	TARGET := ntrviewer.exe
	NASM := -DWIN64 -fwin64
else
	LDLIBS := -static-libgcc -static-libstdc++ -Llib -Wl,-Bstatic -lSDL2
	TARGET := ntrviewer
	NASM := -DELF -felf64
endif

GL_OBJ := libGLAD.o libNK_SDL_GL3.o libNK_SDL_GLES2.o libNK_SDL_renderer.o ui_common_sdl.o ui_renderer_sdl.o ui_renderer_ogl.o ui_main_nk.o ntr_common.o ntr_hb.o ntr_rp.o fsr/fsr_main.o fsr/image_utils.o
ifeq ($(OS),Windows_NT)
GL_OBJ += libGLAD_WGL.o libNK_D3D11.o ui_renderer_d3d11.o ui_compositor_csc.o ntrviewer.res.o
endif
LDLIBS += -lncnn -fopenmp -lglslang -lMachineIndependent -lOSDependent -lGenericCodeGen -lglslang-default-resource-limits -lSPIRV -lSPIRV-Tools-opt -lSPIRV-Tools

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
else
JT_OBJ :=
JT_OBJ_S :=

LDLIBS += -lturbojpeg
endif

NK_SRC := $(wildcard nuklear/*.c)
NK_OBJ := $(NK_SRC:.c=.o)

FEC_SRC := $(wildcard fecal/*.cpp)
FEC_OBJ := $(FEC_SRC:.cpp=.o) fecal/gf256_ssse3.o fecal/gf256_avx2.o fecal/gf256_ssse3_avx2.o

TARGET_OBJ := main.o rp_syn.o ikcp.o $(GL_OBJ) $(JT_OBJ) $(JT_OBJ_S) $(FEC_OBJ) $(NK_OBJ)
TARGET_DEP := $(TARGET_OBJ:.o=.d)

$(TARGET): $(TARGET_OBJ)
	$(CXX) $^ -o $@ $(CFLAGS) $(LDLIBS) $(LDFLAGS)

CC_JT = $(CC) $< -o $@ -c $(CFLAGS) $(CPPFLAGS) -Ijpeg_turbo -Ijpeg_turbo/include -Wno-stringop-overflow -Wno-unused-parameter -Wno-sign-compare

jpeg_turbo/%.o: jpeg_turbo/%.c
	$(CC_JT) -DBMP_SUPPORTED -DGIF_SUPPORTED -DPPM_SUPPORTED -Wno-clobbered

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
	nasm $< -o $@ $(NASM) -D__x86_64__ -Ijpeg_turbo/simd/nasm -Ijpeg_turbo/simd

%.o: %.cpp
	$(CXX) $< -o $@ -c $(CFLAGS) $(CPPFLAGS) -w

ntrviewer.res.o: win_manifest.rc win_manifest.xml
	windres --input $< --output $@ --output-format=coff

realcugan_lib.o: realcugan-ncnn-vulkan/lib.cpp
	$(CXX) realcugan-ncnn-vulkan/lib.cpp -o $@ -c $(CFLAGS) $(CPPFLAGS) -Wno-attributes

realcugan.o: realcugan-ncnn-vulkan/realcugan.cpp
	$(CXX) realcugan-ncnn-vulkan/realcugan.cpp -o $@ -c $(CFLAGS) $(CPPFLAGS) -Wno-attributes

fecal/gf256.o: fecal/gf256.cpp
	$(CXX) $< -o $@ -c $(CFLAGS) $(CPPFLAGS) -Wno-implicit-fallthrough -DGF256_TARGET_MOBILE

fecal/gf256_ssse3.o: fecal/gf256.cpp
	$(CXX) $< -o $@ -c $(CFLAGS) $(CPPFLAGS) -Wno-implicit-fallthrough -mssse3 -DGF_SUFFIX=_ssse3

fecal/gf256_avx2.o: fecal/gf256.cpp
	$(CXX) $< -o $@ -c $(CFLAGS) $(CPPFLAGS) -Wno-implicit-fallthrough -mavx2 -DGF_SUFFIX=_avx2

fecal/gf256_ssse3_avx2.o: fecal/gf256.cpp
	$(CXX) $< -o $@ -c $(CFLAGS) $(CPPFLAGS) -Wno-implicit-fallthrough -mssse3 -mavx2 -DGF_SUFFIX=_ssse3_avx2

fecal/FecalDecoder.o: fecal/FecalDecoder.cpp
	$(CXX) $< -o $@ -c $(CFLAGS) $(CPPFLAGS) -Wno-restrict

nuklear/%.o: nuklear/%.c
	$(CC) $< -o $@ -c $(CFLAGS) $(CPPFLAGS) -Wno-unused-function -std=c89

nuklear/nuklear_font.o: nuklear/nuklear_font.c
	$(CC) $< -o $@ -c $(CFLAGS) $(CPPFLAGS) -Wno-unused-function

nuklear/stb_%.o: nuklear/stb_%.c
	$(CC) $< -o $@ -c $(CFLAGS) $(CPPFLAGS)

%.o: %.c
	$(CC) $< -o $@ -c $(CFLAGS) $(CPPFLAGS) -D_GNU_SOURCE

-include $(TARGET_DEP)

clean:
	-$(RM) $(TARGET)
	-$(RM) *.o jpeg_turbo/jpeg8/*.o jpeg_turbo/jpeg12/*.o jpeg_turbo/jpeg16/*.o jpeg_turbo/*.o jpeg_turbo/simd/x86_64/*.o fsr/*.o fecal/*.o nuklear/*.o
	-$(RM) *.d jpeg_turbo/jpeg8/*.d jpeg_turbo/jpeg12/*.d jpeg_turbo/jpeg16/*.d jpeg_turbo/*.d jpeg_turbo/simd/x86_64/*.d fsr/*.d fecal/*.d nuklear/*.d
