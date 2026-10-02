// The stb_image subset msga-next uses; shared by the implementation file and
// every includer so declarations and definitions agree.
#pragma once
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS // callers only need success/failure
#define STBI_NO_SIMD            // the SSE2 IDCT costs code for speed chat images don't need
#define STBI_MAX_DIMENSIONS 16384
#define STBI_ASSERT(x) ((void)0)
