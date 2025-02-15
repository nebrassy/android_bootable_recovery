/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "graphics.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <memory>

#include <android-base/properties.h>

#ifdef BOARD_USE_CUSTOM_RECOVERY_FONT
#include BOARD_USE_CUSTOM_RECOVERY_FONT
#else
#include "font_10x18.h"
#endif

#include "graphics_drm.h"
#include "graphics_fbdev.h"
#include "minui/minui.h"

static GRFont* gr_font = nullptr;
static MinuiBackend* gr_backend = nullptr;

static int overscan_offset_x = 0;
static int overscan_offset_y = 0;

#ifdef TW_NO_MINUI_CUSTOM_FONTS
static unsigned char gr_current_r = 255;
static unsigned char gr_current_g = 255;
static unsigned char gr_current_b = 255;
#endif
static unsigned char gr_current_a = 255;
static unsigned char rgb_555[2];
static unsigned char gr_current_r5 = 31;
static unsigned char gr_current_g5 = 63;
static unsigned char gr_current_b5 = 31;

static uint32_t gr_current = ~0;

// gr_draw is owned by backends.
static GRSurface* gr_draw = nullptr;
static GRRotation rotation = GRRotation::NONE;
static PixelFormat pixel_format = PixelFormat::UNKNOWN;
// The graphics backend list that provides fallback options for the default backend selection.
// For example, it will fist try DRM, then try FBDEV if DRM is unavailable.
constexpr auto default_backends = { GraphicsBackend::DRM, GraphicsBackend::FBDEV };

static bool outside(int x, int y) {
  auto swapped = (rotation == GRRotation::LEFT || rotation == GRRotation::RIGHT);
  return x < 0 || x >= (swapped ? gr_draw->height : gr_draw->width) || y < 0 ||
         y >= (swapped ? gr_draw->width : gr_draw->height);
}

#ifdef TW_NO_MINUI_CUSTOM_FONTS
int gr_measure(const char *s)
{
    return gr_font->char_width * strlen(s);
}

void gr_font_size(int *x, int *y)
{
    *x = gr_font->char_width;
    *y = gr_font->char_height;
}
#else // TW_USE_MINUI_CUSTOM_FONTS
const GRFont* gr_sys_font() {
  return gr_font;
}

PixelFormat gr_pixel_format() {
  return pixel_format;
}

int gr_measure(const GRFont* font, const char* s) {
  if (font == nullptr) {
    return -1;
  }

  return font->char_width * strlen(s);
}

int gr_font_size(const GRFont* font, int* x, int* y) {
  if (font == nullptr) {
    return -1;
  }

  *x = font->char_width;
  *y = font->char_height;
  return 0;
}
#endif // TW_NO_MINUI_CUSTOM_FONTS

void blend_16bpp(unsigned char* px, unsigned r5, unsigned g5, unsigned b5, unsigned char a)
{
    unsigned char orig[2];
    orig[0] = px[0];
    orig[1] = px[1];

    /* This code is a little easier to read
    unsigned oldred = (orig[1] >> 3);
    unsigned oldgreen = (((orig[0] >> 5) << 3) + (orig[1] & 0x7));
    unsigned oldblue = (orig[0] & 0x1F);

    unsigned newred = (oldred * (255-a) + r5 * a) / 255;
    unsigned newgreen = (oldgreen * (255-a) + g5 * a) / 255;
    unsigned newblue = (oldblue * (255-a) + b5 * a) / 255;
    */

    unsigned newred = ((orig[1] >> 3) * (255-a) + r5 * a) / 255;
    unsigned newgreen = ((((orig[0] >> 5) << 3) + (orig[1] & 0x7)) * (255-a) + g5 * a) / 255;
    unsigned newblue = ((orig[0] & 0x1F) * (255-a) + b5 * a) / 255;

    *px++ = (newgreen << 5) + (newblue);
    *px++ = (newred << 3) + (newgreen >> 3);
}

#ifdef TW_NO_MINUI_CUSTOM_FONTS
static void text_blend_old(unsigned char* src_p, int src_row_bytes,
                       unsigned char* dst_p, int dst_row_bytes,
                       int width, int height)
{
    for (int j = 0; j < height; ++j) {
        unsigned char* sx = src_p;
        unsigned char* px = dst_p;
        for (int i = 0; i < width; ++i) {
            unsigned char a = *sx++;
            if (gr_current_a < 255) a = ((int)a * gr_current_a) / 255;
            if (a == 255) {
                if (gr_draw->pixel_bytes == 2) {
                    *px++ = rgb_555[0];
                    *px++ = rgb_555[1];
                } else {
                    *px++ = gr_current_r;
                    *px++ = gr_current_g;
                    *px++ = gr_current_b;
                    px++;
                }
            } else if (a > 0) {
                if (gr_draw->pixel_bytes == 2) {
                    blend_16bpp(px, gr_current_r5, gr_current_g5, gr_current_b5, a);
                    px += gr_draw->pixel_bytes;
                } else {
                    *px = (*px * (255-a) + gr_current_r * a) / 255;
                    ++px;
                    *px = (*px * (255-a) + gr_current_g * a) / 255;
                    ++px;
                    *px = (*px * (255-a) + gr_current_b * a) / 255;
                    ++px;
                    ++px;
                }
            } else {
                px += gr_draw->pixel_bytes;
            }
        }
        src_p += src_row_bytes;
        dst_p += dst_row_bytes;
    }
}
#endif // TW_NO_MINUI_CUSTOM_FONTS

// Blends gr_current onto pix value, assumes alpha as most significant byte.
static inline uint32_t pixel_blend_argb(uint8_t alpha, uint32_t pix) {
  if (alpha == 255) return gr_current;
  if (alpha == 0) return pix;
  uint32_t pix_r = pix & 0xff;
  uint32_t pix_g = pix & 0xff00;
  uint32_t pix_b = pix & 0xff0000;
  uint32_t cur_r = gr_current & 0xff;
  uint32_t cur_g = gr_current & 0xff00;
  uint32_t cur_b = gr_current & 0xff0000;

  uint32_t out_r = (pix_r * (255 - alpha) + cur_r * alpha) / 255;
  uint32_t out_g = (pix_g * (255 - alpha) + cur_g * alpha) / 255;
  uint32_t out_b = (pix_b * (255 - alpha) + cur_b * alpha) / 255;

  return (out_r & 0xff) | (out_g & 0xff00) | (out_b & 0xff0000) | (gr_current & 0xff000000);
}

static inline uint32_t pixel_blend_rgba(uint8_t alpha, uint32_t pix) {
  if (alpha == 255) return gr_current;
  if (alpha == 0) return pix;
  uint32_t pix_r = pix & 0xff00;
  uint32_t pix_g = pix & 0xff0000;
  uint32_t pix_b = pix & 0xff000000;
  uint32_t cur_r = gr_current & 0xff00;
  uint32_t cur_g = gr_current & 0xff0000;
  uint32_t cur_b = gr_current & 0xff000000;

  uint32_t out_r = (pix_r * (255 - alpha) + cur_r * alpha) / 255;
  uint32_t out_g = (pix_g * (255 - alpha) + cur_g * alpha) / 255;
  uint32_t out_b = (pix_b * (255 - alpha) + cur_b * alpha) / 255;

  return (gr_current & 0xff) | (out_r & 0xff00) | (out_g & 0xff0000) | (out_b & 0xff000000);
}

static inline uint32_t pixel_blend(uint8_t alpha, uint32_t pix) {
  if (pixel_format == PixelFormat::RGBA) {
    return pixel_blend_rgba(alpha, pix);
  }
  return pixel_blend_argb(alpha, pix);
}

static inline uint32_t get_alphamask() {
  if (pixel_format == PixelFormat::RGBA) {
    return 0x000000ff;
  }
  return 0xff000000;
}

static inline uint8_t get_alpha_shift() {
  if (pixel_format == PixelFormat::RGBA) {
    return 0;
  }
  return 24;
}

static inline uint8_t get_alpha(uint32_t pix) {
  return static_cast<uint8_t>((pix & (gr_current & get_alphamask())) >> get_alpha_shift());
}

// Increments pixel pointer right, with current rotation.
static void incr_x(uint32_t** p, int row_pixels) {
  if (rotation == GRRotation::LEFT) {
    *p = *p - row_pixels;
  } else if (rotation == GRRotation::RIGHT) {
    *p = *p + row_pixels;
  } else if (rotation == GRRotation::DOWN) {
    *p = *p - 1;
  } else {  // GRRotation::NONE
    *p = *p + 1;
  }
}

// Increments pixel pointer down, with current rotation.
static void incr_y(uint32_t** p, int row_pixels) {
  if (rotation == GRRotation::LEFT) {
    *p = *p + 1;
  } else if (rotation == GRRotation::RIGHT) {
    *p = *p - 1;
  } else if (rotation == GRRotation::DOWN) {
    *p = *p - row_pixels;
  } else {  // GRRotation::NONE
    *p = *p + row_pixels;
  }
}

// Returns pixel pointer at given coordinates with rotation adjustment.
static uint32_t* PixelAt(GRSurface* surface, int x, int y, int row_pixels) {
  switch (rotation) {
    case GRRotation::NONE:
      return reinterpret_cast<uint32_t*>(surface->data()) + y * row_pixels + x;
    case GRRotation::RIGHT:
      return reinterpret_cast<uint32_t*>(surface->data()) + x * row_pixels + (surface->width - y);
    case GRRotation::DOWN:
      return reinterpret_cast<uint32_t*>(surface->data()) + (surface->height - 1 - y) * row_pixels +
             (surface->width - 1 - x);
    case GRRotation::LEFT:
      return reinterpret_cast<uint32_t*>(surface->data()) + (surface->height - 1 - x) * row_pixels +
             y;
    default:
      printf("invalid rotation %d", static_cast<int>(rotation));
  }
  return nullptr;
}

static void TextBlend(const uint8_t* src_p, int src_row_bytes, uint32_t* dst_p, int dst_row_pixels,
                      int width, int height) {
  uint8_t alpha_current = get_alpha(gr_current);
  for (int j = 0; j < height; ++j) {
    const uint8_t* sx = src_p;
    uint32_t* px = dst_p;
    for (int i = 0; i < width; ++i, incr_x(&px, dst_row_pixels)) {
      uint8_t a = *sx++;
      if (alpha_current < 255) a = (static_cast<uint32_t>(a) * alpha_current) / 255;
      *px = pixel_blend(a, *px);
    }
    src_p += src_row_bytes;
    incr_y(&dst_p, dst_row_pixels);
  }
}


#ifdef TW_NO_MINUI_CUSTOM_FONTS
void gr_text(int x, int y, const char *s, bool bold)
{
    GRFont* font = gr_font;

    if (!font->texture || gr_current_a == 0) return;

    bold = bold && (font->texture->height != font->char_height);

    x += overscan_offset_x;
    y += overscan_offset_y;

    unsigned char ch;
    while ((ch = *s++)) {
        if (outside(x, y) || outside(x+font->char_width-1, y+font->char_height-1)) break;

        if (ch < ' ' || ch > '~') {
            ch = '?';
        }

        unsigned char* src_p = font->texture->data + ((ch - ' ') * font->char_width) +
                               (bold ? font->char_height * font->texture->row_bytes : 0);
        unsigned char* dst_p = gr_draw->data + y*gr_draw->row_bytes + x*gr_draw->pixel_bytes;

        text_blend_old(src_p, font->texture->row_bytes,
                       dst_p, gr_draw->row_bytes,
                       font->char_width, font->char_height);

        x += font->char_width;
    }
}
#else //TW_NO_MINUI_CUSTOM_FONTS
void gr_text(const GRFont* font, int x, int y, const char* s, bool bold) {
  if (!font || !font->texture || (gr_current & get_alphamask()) == 0) return;

  if (font->texture->pixel_bytes != 1) {
    printf("gr_text: font has wrong format\n");
    return;
  }

  bold = bold && (font->texture->height != font->char_height);

  x += overscan_offset_x;
  y += overscan_offset_y;

  unsigned char ch;
  while ((ch = *s++)) {
    if (outside(x, y) || outside(x + font->char_width - 1, y + font->char_height - 1)) break;

    if (ch < ' ' || ch > '~') {
      ch = '?';
    }

    int row_pixels = gr_draw->row_bytes / gr_draw->pixel_bytes;
    const uint8_t* src_p = font->texture->data() + ((ch - ' ') * font->char_width) +
                           (bold ? font->char_height * font->texture->row_bytes : 0);
    uint32_t* dst_p = PixelAt(gr_draw, x, y, row_pixels);

    TextBlend(src_p, font->texture->row_bytes, dst_p, row_pixels, font->char_width,
              font->char_height);

    x += font->char_width;
  }
}
#endif //TW_NO_MINUI_CUSTOM_FONTS

void gr_texticon(int x, int y, const GRSurface* icon) {
  if (icon == nullptr) return;

  if (icon->pixel_bytes != 1) {
    printf("gr_texticon: source has wrong format\n");
    return;
  }

  x += overscan_offset_x;
  y += overscan_offset_y;

  if (outside(x, y) || outside(x + icon->width - 1, y + icon->height - 1)) return;

  int row_pixels = gr_draw->row_bytes / gr_draw->pixel_bytes;
  const uint8_t* src_p = icon->data();
  uint32_t* dst_p = PixelAt(gr_draw, x, y, row_pixels);
  TextBlend(src_p, icon->row_bytes, dst_p, row_pixels, icon->width, icon->height);
}

void gr_convert_rgb_555(unsigned char r, unsigned char g, unsigned char b)
{
    gr_current_r5 = (((r & 0xFF) * 0x1F) + 0x7F) / 0xFF;
    gr_current_g5 = (((g & 0xFF) * 0x3F) + 0x7F) / 0xFF;
    gr_current_b5 = (((b & 0xFF) * 0x1F) + 0x7F) / 0xFF;

    rgb_555[0] = (gr_current_g5 << 5) + (gr_current_b5);
    rgb_555[1] = (gr_current_r5 << 3) + (gr_current_g5 >> 3);
}

void gr_color(unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
#ifdef TW_NO_MINUI_CUSTOM_FONTS
#if defined(RECOVERY_ABGR) || defined(RECOVERY_BGRA)
  gr_current_r = b;
  gr_current_g = g;
  gr_current_b = r;
#else
  gr_current_r = r;
  gr_current_g = g;
  gr_current_b = b;
#endif
#endif

  if (gr_draw->pixel_bytes == 2) {
	gr_current_a = a;
    gr_convert_rgb_555(r, g, b);
    return;
  }

  uint32_t r32 = r, g32 = g, b32 = b, a32 = a;
  if (pixel_format == PixelFormat::ARGB || pixel_format == PixelFormat::BGRA) {
    gr_current = (a32 << 24) | (r32 << 16) | (g32 << 8) | b32;
  } else if (pixel_format == PixelFormat::RGBA) {
    gr_current = (b32 << 24) | (g32 << 16) | (r32 << 8) | a32;
  } else {
    gr_current = (a32 << 24) | (b32 << 16) | (g32 << 8) | r32;
  }
}

void gr_clear() {
  if (gr_draw->pixel_bytes == 2) {
    gr_fill(0, 0, gr_fb_width(), gr_fb_height());
    return;
  }

  // This code only works on 32bpp devices
  if ((gr_current & 0xff) == ((gr_current >> 8) & 0xff) &&
      (gr_current & 0xff) == ((gr_current >> 16) & 0xff) &&
      (gr_current & 0xff) == ((gr_current >> 24) & 0xff) &&
      gr_draw->row_bytes == gr_draw->width * gr_draw->pixel_bytes) {
    memset(gr_draw->data(), gr_current & 0xff, gr_draw->height * gr_draw->row_bytes);
  } else {
    uint32_t* px = reinterpret_cast<uint32_t*>(gr_draw->data());
    int row_diff = gr_draw->row_bytes / gr_draw->pixel_bytes - gr_draw->width;
    for (int y = 0; y < gr_draw->height; ++y) {
      for (int x = 0; x < gr_draw->width; ++x) {
        *px++ = gr_current;
      }
      px += row_diff;
    }
  }
}

void gr_fill(int x1, int y1, int x2, int y2) {
  x1 += overscan_offset_x;
  y1 += overscan_offset_y;

  x2 += overscan_offset_x;
  y2 += overscan_offset_y;

  if (outside(x1, y1) || outside(x2 - 1, y2 - 1)) return;

  int row_pixels = gr_draw->row_bytes / gr_draw->pixel_bytes;
  uint32_t* p = PixelAt(gr_draw, x1, y1, row_pixels);
  uint8_t alpha = get_alpha(gr_current);
  if (alpha > 0) {
    for (int y = y1; y < y2; ++y) {
      uint32_t* px = p;
      for (int x = x1; x < x2; ++x) {
        *px = pixel_blend(alpha, *px);
        incr_x(&px, row_pixels);
      }
      incr_y(&p, row_pixels);
    }
  }
}

void gr_blit_32to16(const GRSurface* source, int sx, int sy, int w, int h, int dx, int dy) {
  dx += overscan_offset_x;
  dy += overscan_offset_y;

  if (outside(dx, dy) || outside(dx+w-1, dy+h-1)) return;

  unsigned char* src_p = (unsigned char*) source->data() + sy*source->row_bytes + sx*source->pixel_bytes;
  unsigned char* dst_p = gr_draw->data() + dy*gr_draw->row_bytes + dx*gr_draw->pixel_bytes;

  int i, j;
  for (i = 0; i < h; ++i) {
    unsigned char* spx = src_p;
    unsigned char* dpx = dst_p;

    for (j = 0; j < w; ++j) {
      unsigned a = spx[3];

      if (a == 0) {
        spx += source->pixel_bytes;
        dpx += gr_draw->pixel_bytes;
      } else {
        unsigned r5 = (((*spx++ & 0xFF) * 0x1F) + 0x7F) / 0xFF;
        unsigned g5 = (((*spx++ & 0xFF) * 0x3F) + 0x7F) / 0xFF;
        unsigned b5 = (((*spx++ & 0xFF) * 0x1F) + 0x7F) / 0xFF;
        spx++;
        if (a == 255) {
          *dpx++ = (g5 << 5) + (b5);
          *dpx++ = (r5 << 3) + (g5 >> 3);
        } else {
          blend_16bpp(dpx, r5, g5, b5, a);
          spx += source->pixel_bytes;
        }
      }
    }
    src_p += source->row_bytes;
    dst_p += gr_draw->row_bytes;
  }
}

void gr_blit(const GRSurface* source, int sx, int sy, int w, int h, int dx, int dy) {
  if (source == nullptr) return;

  if (gr_draw->pixel_bytes != source->pixel_bytes) {
    if (gr_draw->pixel_bytes == 2 && source->pixel_bytes == 4) {
      gr_blit_32to16(source, sx, sy, w, h, dx, dy);
      return;
    } else {
      printf("gr_blit: source has wrong format\n");
      return;
    }
  }

  dx += overscan_offset_x;
  dy += overscan_offset_y;

  if (outside(dx, dy) || outside(dx + w - 1, dy + h - 1)) return;

  if (rotation != GRRotation::NONE) {
    int src_row_pixels = source->row_bytes / source->pixel_bytes;
    int row_pixels = gr_draw->row_bytes / gr_draw->pixel_bytes;
    const uint32_t* src_py =
        reinterpret_cast<const uint32_t*>(source->data()) + sy * source->row_bytes / 4 + sx;
    uint32_t* dst_py = PixelAt(gr_draw, dx, dy, row_pixels);

    for (int y = 0; y < h; y += 1) {
      const uint32_t* src_px = src_py;
      uint32_t* dst_px = dst_py;
      for (int x = 0; x < w; x += 1) {
        *dst_px = *src_px++;
        incr_x(&dst_px, row_pixels);
      }
      src_py += src_row_pixels;
      incr_y(&dst_py, row_pixels);
    }
  } else {
    const uint8_t* src_p = source->data() + sy * source->row_bytes + sx * source->pixel_bytes;
    uint8_t* dst_p = gr_draw->data() + dy * gr_draw->row_bytes + dx * gr_draw->pixel_bytes;

    for (int i = 0; i < h; ++i) {
      memcpy(dst_p, src_p, w * source->pixel_bytes);
      src_p += source->row_bytes;
      dst_p += gr_draw->row_bytes;
    }
  }
}

unsigned int gr_get_width(const GRSurface* surface) {
  if (surface == nullptr) {
    return 0;
  }
  return surface->width;
}

unsigned int gr_get_height(const GRSurface* surface) {
  if (surface == nullptr) {
    return 0;
  }
  return surface->height;
}

#ifdef TW_NO_MINUI_CUSTOM_FONTS
static void gr_init_font(void)
{
    gr_font = reinterpret_cast<GRFont*>(calloc(sizeof(*gr_font), 1));

    int res = res_create_alpha_surface("font", &(gr_font->texture));
    if (res == 0) {
        // The font image should be a 96x2 array of character images.  The
        // columns are the printable ASCII characters 0x20 - 0x7f.  The
        // top row is regular text; the bottom row is bold.
        gr_font->char_width = gr_font->texture->width / 96;
        gr_font->char_height = gr_font->texture->height / 2;
    } else {
        printf("failed to read font: res=%d\n", res);

        // fall back to the compiled-in font.
        gr_font->texture = reinterpret_cast<GRSurface*>(malloc(sizeof(*gr_font->texture)));
        gr_font->texture->width = font.width;
        gr_font->texture->height = font.height;
        gr_font->texture->row_bytes = font.width;
        gr_font->texture->pixel_bytes = 1;

        unsigned char* bits = reinterpret_cast<unsigned char*>(malloc(font.width * font.height));
        gr_font->texture->data = reinterpret_cast<unsigned char*>(bits);

        unsigned char data;
        unsigned char* in = font.rundata;
        while((data = *in++)) {
            memset(bits, (data & 0x80) ? 255 : 0, data & 0x7f);
            bits += (data & 0x7f);
        }

        gr_font->char_width = font.char_width;
        gr_font->char_height = font.char_height;
    }
}

void gr_set_font(__attribute__ ((unused))const char* name) {
	//this cm function is made to change font. Don't care, just init the font:
	gr_init_font();
	return;
}
#else // TW_NO_MINUI_CUSTOM_FONTS
int gr_init_font(const char* name, GRFont** dest) {
  GRFont* font = static_cast<GRFont*>(calloc(1, sizeof(*gr_font)));
  if (font == nullptr) {
    return -1;
  }

  int res = res_create_alpha_surface(name, &(font->texture));
  if (res < 0) {
    free(font);
    return res;
  }

  // The font image should be a 96x2 array of character images.  The
  // columns are the printable ASCII characters 0x20 - 0x7f.  The
  // top row is regular text; the bottom row is bold.
  font->char_width = font->texture->width / 96;
  font->char_height = font->texture->height / 2;

  *dest = font;

  return 0;
}
#endif // TW_NO_MINUI_CUSTOM_FONTS

void gr_flip() {
  gr_draw = gr_backend->Flip();
}

std::unique_ptr<MinuiBackend> create_backend(GraphicsBackend backend) {
  switch (backend) {
    case GraphicsBackend::DRM:
      return std::make_unique<MinuiBackendDrm>();
    case GraphicsBackend::FBDEV:
      return std::make_unique<MinuiBackendFbdev>();
    default:
      return nullptr;
  }
}

int gr_init() {
  return gr_init(default_backends);
}

int gr_init(__attribute__((unused)) std::initializer_list<GraphicsBackend> backends) {
  // pixel_format needs to be set before loading any resources or initializing backends.
  std::string format = android::base::GetProperty("ro.minui.pixel_format", "");
  if (format == "ABGR_8888") {
    pixel_format = PixelFormat::ABGR;
  } else if (format == "RGBX_8888") {
    pixel_format = PixelFormat::RGBX;
  } else if (format == "ARGB_8888") {
    pixel_format = PixelFormat::ARGB;
  } else if (format == "BGRA_8888") {
    pixel_format = PixelFormat::BGRA;
  } else if (format == "RGBA_8888") {
    pixel_format = PixelFormat::RGBA;
  } else {
    pixel_format = PixelFormat::UNKNOWN;
  }

  int ret = gr_init_font("font", &gr_font);
  if (ret != 0) {
    printf("Failed to init font: %d, continuing graphic backend initialization without font file\n",
           ret);
  }

  auto backend = std::unique_ptr<MinuiBackend>{ std::make_unique<MinuiBackendDrm>() };
  gr_draw = backend->Init();

#ifdef MSM_BSP
    if (gr_draw) {
        printf("Using overlay graphics.\n");
    }
#endif


    if (!gr_draw) {
        backend = std::make_unique<MinuiBackendDrm>();
        gr_draw = backend->Init();
        if (gr_draw)
            printf("Using drm graphics.\n");
    }

    if (!gr_draw) {
        backend = std::make_unique<MinuiBackendFbdev>();
        gr_draw = backend->Init();
        if (gr_draw)
            printf("Using fbdev graphics.\n");
    }

  if (!gr_draw) {
    return -1;
  }


  int overscan_percent = android::base::GetIntProperty("ro.minui.overscan_percent", 0);
  overscan_offset_x = gr_draw->width * overscan_percent / 100;
  overscan_offset_y = gr_draw->height * overscan_percent / 100;

  gr_flip();
  gr_flip();
  if (!gr_draw) {
    printf("gr_init: gr_draw becomes nullptr after gr_flip\n");
    return -1;
  }

  std::string rotation_str =
      android::base::GetProperty("ro.minui.default_rotation", "ROTATION_NONE");
  if (rotation_str == "ROTATION_RIGHT") {
    gr_rotate(GRRotation::RIGHT);
  } else if (rotation_str == "ROTATION_DOWN") {
    gr_rotate(GRRotation::DOWN);
  } else if (rotation_str == "ROTATION_LEFT") {
    gr_rotate(GRRotation::LEFT);
  } else {  // "ROTATION_NONE" or unknown string
    gr_rotate(GRRotation::NONE);
  }

  if (gr_draw->pixel_bytes != 4) {
    printf("gr_init: Only 4-byte pixel formats supported\n");
  }

  return 0;
}

void gr_exit() {
  delete gr_backend;
  gr_backend = nullptr;

  delete gr_font;
  gr_font = nullptr;
}

int gr_fb_width() {
  return (rotation == GRRotation::LEFT || rotation == GRRotation::RIGHT)
             ? gr_draw->height - 2 * overscan_offset_y
             : gr_draw->width - 2 * overscan_offset_x;
}

int gr_fb_height() {
  return (rotation == GRRotation::LEFT || rotation == GRRotation::RIGHT)
             ? gr_draw->width - 2 * overscan_offset_x
             : gr_draw->height - 2 * overscan_offset_y;
}

void gr_fb_blank(bool blank) {
  gr_backend->Blank(blank);
}

void gr_fb_blank(bool blank, int index) {
  gr_backend->Blank(blank, static_cast<MinuiBackend::DrmConnector>(index));
}

void gr_rotate(GRRotation rot) {
  rotation = rot;
}

bool gr_has_multiple_connectors() {
  return gr_backend->HasMultipleConnectors();
}
