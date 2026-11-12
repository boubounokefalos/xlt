/* Only what gfx.c uses; sceGuTexMode/TexImage/DrawArray/Clear are implemented
 * by tests/saver_preview.c (a small software rasterizer). */
#ifndef HOST_PSPGU_H
#define HOST_PSPGU_H
#define GU_PSM_5650 0
#define GU_PSM_5551 1
#define GU_PSM_4444 2
#define GU_PSM_8888 3
#define GU_SPRITES 6
#define GU_TEXTURE_16BIT (2 << 0)
#define GU_COLOR_8888 (7 << 2)
#define GU_VERTEX_16BIT (2 << 7)
#define GU_TRANSFORM_2D (1 << 23)
#define GU_TRUE 1
#define GU_FALSE 0
#define GU_DIRECT 0
#define GU_COLOR_BUFFER_BIT 1
#define GU_SCISSOR_TEST 0
#define GU_DEPTH_TEST 0
#define GU_CULL_FACE 0
#define GU_DITHER 0
#define GU_BLEND 0
#define GU_TEXTURE_2D 0
#define GU_ADD 0
#define GU_SRC_ALPHA 0
#define GU_ONE_MINUS_SRC_ALPHA 0
#define GU_TFX_MODULATE 0
#define GU_TCC_RGBA 1
#define GU_TCC_RGB 0
#define GU_NEAREST 0
#define GU_LINEAR 1
#define GU_CLAMP 1
#define GU_GREATER 4
#define GU_ALPHA_TEST 0
#define sceGuSetDither(m) ((void)(m))
#define sceGuAlphaFunc(a, b, c) ((void)0)
void sceGuTexMode(int psm, int maxmips, int a2, int swizzle);
void sceGuTexImage(int mip, int w, int h, int tbw, const void *tbp);
void sceGuDrawArray(int prim, int vtype, int count, const void *ind, const void *vtx);
void sceGuClearColor(unsigned c);
void sceGuClear(int flags);
void sceGuTexFunc(int tfx, int tcc);
#define sceGuInit() ((void)0)
#define sceGuTerm() ((void)0)
#define sceGuStart(a, b) ((void)0)
#define sceGuFinish() ((void)0)
#define sceGuSync(a, b) ((void)0)
#define sceGuSwapBuffers() ((void)0)
#define sceGuDrawBuffer(a, b, c) ((void)0)
#define sceGuDispBuffer(a, b, c, d) ((void)0)
#define sceGuDepthBuffer(a, b) ((void)0)
#define sceGuOffset(a, b) ((void)0)
#define sceGuViewport(a, b, c, d) ((void)0)
#define sceGuScissor(a, b, c, d) ((void)0)
#define sceGuEnable(a) ((void)0)
#define sceGuDisable(a) ((void)0)
#define sceGuDepthMask(a) ((void)0)
#define sceGuBlendFunc(a, b, c, d, e) ((void)0)
#define sceGuTexFilter(a, b) ((void)0)
#define sceGuTexWrap(a, b) ((void)0)
#define sceGuTexScale(a, b) ((void)0)
#define sceGuTexOffset(a, b) ((void)0)
#define sceGuTexFlush() ((void)0)
#define sceGuDisplay(a) ((void)0)
#endif
