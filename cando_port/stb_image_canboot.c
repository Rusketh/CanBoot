/*
 * stb_image vendored shim for canboot. We compile stb_image.h with
 * STB_IMAGE_IMPLEMENTATION here so its ~3 KLOC body lands in a
 * single translation unit, away from the rest of the build. The
 * library is fed images via stbi_load_from_memory; no FILE* IO is
 * pulled in. Memory routes through picolibc's malloc/free, which we
 * already link.
 *
 * Decoders enabled: PNG, JPEG, BMP. Other formats stb_image supports
 * (GIF/PSD/TGA/PIC/PNM/HDR) are disabled at compile time so we
 * don't drag in their tables/Huffman state.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#define STBI_NO_STDIO
#define STBI_NO_GIF
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ASSERT(x) ((void)0)

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
