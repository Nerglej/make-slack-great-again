// stb_image implementation, compiled once (-Os, see ../../gfx/CMakeLists.txt).
#include "stb_config.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// msga: frame-by-frame GIF decoding (declared in stb_config.h).
// stbi_load_gif_from_memory keeps every composited frame in one growing
// buffer (frames x w x h x 4); this hands each frame to `fn` as soon as it is
// composited and keeps only the frame the disposal methods may look back at.
// The compositing is stbi__load_gif_main's: the same stbi__gif_load_next
// calls with the same two_back frame (the one before the previous).
int msga_gif_frames(
    const unsigned char *buffer,
    int                  len,
    int (*fn)(void *ctx, const unsigned char *rgba, int w, int h, int delayMs),
    void *ctx
) {
   stbi__context s;
   stbi__gif     g;
   stbi_uc      *last = 0, *prev = 0, *two_back = 0;
   size_t        stride = 0;
   int           comp = 0, layers = 0;
   stbi__start_mem(&s, buffer, len);
   if (!stbi__gif_test(&s))
      return 0;
   memset(&g, 0, sizeof(g));
   for (;;) {
      stbi_uc *u = stbi__gif_load_next(&s, &g, &comp, 4, two_back);
      if (u == (stbi_uc *)&s)
         u = 0; // the end of the animation
      if (!u)
         break;
      if (!last) {
         stride = (size_t)g.w * (size_t)g.h * 4;
         last   = (stbi_uc *)stbi__malloc(stride);
         prev   = (stbi_uc *)stbi__malloc(stride);
         if (!last || !prev)
            break;
      }
      ++layers;
      if (layers >= 2) {
         // The next frame's two_back is the one before this: the copy
         // `last` still holds.
         stbi_uc *t = prev;
         prev       = last;
         last       = t;
         two_back   = prev;
      }
      memcpy(last, u, stride);
      if (!fn(ctx, u, g.w, g.h, g.delay))
         break;
   }
   STBI_FREE(g.out);
   STBI_FREE(g.history);
   STBI_FREE(g.background);
   STBI_FREE(last);
   STBI_FREE(prev);
   return layers;
}
