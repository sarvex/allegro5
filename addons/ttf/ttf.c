#include "allegro5/allegro.h"
#ifdef ALLEGRO_CFG_OPENGL
#include "allegro5/allegro_opengl.h"
#endif
#include "allegro5/internal/aintern.h"
#include "allegro5/internal/aintern_vector.h"

#include "allegro5/allegro_ttf.h"
#include "allegro5/internal/aintern_ttf_cfg.h"
#include "allegro5/internal/aintern_dtor.h"
#include "allegro5/internal/aintern_system.h"

#include <ft2build.h>
#include FT_FREETYPE_H

ALLEGRO_DEBUG_CHANNEL("font")


/* Some low-end drivers enable automatic S3TC compression, which
 * requires glTexSubImage2D to only work on multiples of aligned
 * 4x4 pixel blocks with some buggy OpenGL drivers.
 * There's not much we can do about that in general - if the user
 * locks a portion of a bitmap not conformin to this it will fail
 * with such a driver.
 * 
 * However in many programs this is no problem at all safe for rendering
 * glyphs and simply aligning to 4 pixels here fixes it.
 */
#define ALIGN_TO_4_PIXEL


#define RANGE_SIZE   128


typedef struct REGION
{
   short x;
   short y;
   short w;
   short h;
} REGION;


typedef struct ALLEGRO_TTF_GLYPH_DATA
{
   ALLEGRO_BITMAP *page_bitmap;
   REGION region;
   short offset_x;
   short offset_y;
   short advance;
} ALLEGRO_TTF_GLYPH_DATA;


typedef struct ALLEGRO_TTF_GLYPH_RANGE
{
   int32_t range_start;
   ALLEGRO_TTF_GLYPH_DATA *glyphs;  /* [RANGE_SIZE] */
} ALLEGRO_TTF_GLYPH_RANGE;


typedef struct ALLEGRO_TTF_FONT_DATA
{
   FT_Face face;
   int flags;
   bool no_premultiply_alpha;
   _AL_VECTOR glyph_ranges;  /* sorted array of of ALLEGRO_TTF_GLYPH_RANGE */

   _AL_VECTOR page_bitmaps;  /* of ALLEGRO_BITMAP pointers */
   int page_pos_x;
   int page_pos_y;
   int page_line_height;
   REGION lock_rect;
   ALLEGRO_LOCKED_REGION *page_lr;
   ALLEGRO_LOCKED_REGION glyph_lr;

   FT_StreamRec stream;
   ALLEGRO_FILE *file;
   unsigned long base_offset;
   unsigned long offset;
} ALLEGRO_TTF_FONT_DATA;


/* globals */
static bool inited;
static FT_Library ft;
static ALLEGRO_FONT_VTABLE vt;


static INLINE int align4(int x)
{
#ifdef ALIGN_TO_4_PIXEL
   return (x + 3) & ~3;
#else
   return x;
#endif
}


static ALLEGRO_TTF_GLYPH_DATA *get_glyph(ALLEGRO_TTF_FONT_DATA *data,
   int ft_index)
{
   ALLEGRO_TTF_GLYPH_RANGE *range;
   int32_t range_start;
   int lo, hi, mid;

   range_start = ft_index - (ft_index % RANGE_SIZE);

   /* Binary search for the range. */
   lo = 0;
   hi = _al_vector_size(&data->glyph_ranges);
   mid = (hi + lo)/2;
   range = NULL;

   while (lo < hi) {
      ALLEGRO_TTF_GLYPH_RANGE *r = _al_vector_ref(&data->glyph_ranges, mid);
      if (r->range_start == range_start) {
         range = r;
         break;
      }
      else if (r->range_start < range_start) {
         lo = mid + 1;
      }
      else {
         hi = mid;
      }
      mid = (hi + lo)/2;
   }

   if (!range) {
      range = _al_vector_alloc_mid(&data->glyph_ranges, mid);
      range->range_start = range_start;
      range->glyphs = al_calloc(RANGE_SIZE, sizeof(ALLEGRO_TTF_GLYPH_DATA));
   }

   return &range->glyphs[ft_index - range_start];
}


static void unlock_current_page(ALLEGRO_TTF_FONT_DATA *data)
{
   if (data->page_lr) {
      ALLEGRO_BITMAP **back = _al_vector_ref_back(&data->page_bitmaps);
      ASSERT(al_is_bitmap_locked(*back));
      al_unlock_bitmap(*back);
      data->page_lr = NULL;
   }
}


// FIXME: Add a special case for when a single glyph rendering won't fit
// into 256x256 pixels.
static ALLEGRO_BITMAP *push_new_page(ALLEGRO_TTF_FONT_DATA *data)
{
    ALLEGRO_BITMAP **back;
    ALLEGRO_BITMAP *page;
    int old_format;
    ALLEGRO_STATE state;

    unlock_current_page(data);

    /* The bitmap will be destroyed when the parent font is destroyed so
     * it is not safe to register a destructor for it.
     */
    _al_push_destructor_owner();
    old_format = al_get_new_bitmap_format();
    al_set_new_bitmap_format(ALLEGRO_PIXEL_FORMAT_ANY_WITH_ALPHA);
    page = al_create_bitmap(256, 256);
    al_set_new_bitmap_format(old_format);
    _al_pop_destructor_owner();

    back = _al_vector_alloc_back(&data->page_bitmaps);
    *back = page;

    /* Sometimes OpenGL will partly sample texels from the border of
     * glyphs. So we better clear the texture to transparency.
     * XXX This is very slow and avoidable with some effort.
     */
    al_store_state(&state, ALLEGRO_STATE_TARGET_BITMAP);
    al_hold_bitmap_drawing(false);
    al_set_target_bitmap(*back);
    al_clear_to_color(al_map_rgba_f(0, 0, 0, 0));
    al_restore_state(&state);

    data->page_pos_x = 0;
    data->page_pos_y = 0;
    data->page_line_height = 0;

    return page;
}


static unsigned char *alloc_glyph_region(ALLEGRO_TTF_FONT_DATA *data,
   int w, int h, bool new, ALLEGRO_TTF_GLYPH_DATA *glyph, bool lock_more)
{
   ALLEGRO_BITMAP *page;
   bool relock;

   if (_al_vector_is_empty(&data->page_bitmaps) || new) {
      page = push_new_page(data);
      relock = true;
   }
   else {
      ALLEGRO_BITMAP **back = _al_vector_ref_back(&data->page_bitmaps);
      page = *back;
      relock = !data->page_lr;
   }

   w = align4(w);
   h = align4(h);

   if (data->page_pos_x + w > al_get_bitmap_width(page)) {
      data->page_pos_y += data->page_line_height + 2;
      data->page_pos_y = align4(data->page_pos_y);
      data->page_pos_x = 0;
      data->page_line_height = 0;
      relock = true;
   }

   if (data->page_pos_y + h > al_get_bitmap_height(page)) {
      return alloc_glyph_region(data, w, h, true, glyph, lock_more);
   }

   glyph->page_bitmap = page;
   glyph->region.x = data->page_pos_x;
   glyph->region.y = data->page_pos_y;
   glyph->region.w = w;
   glyph->region.h = h;

   data->page_pos_x = align4(data->page_pos_x + w + 2);
   if (h > data->page_line_height) {
      data->page_line_height = h;
      relock = true;
   }

   if (relock) {
      unlock_current_page(data);

      data->lock_rect.x = glyph->region.x;
      data->lock_rect.y = glyph->region.y;
      data->lock_rect.h = data->page_line_height;
      /* Do we lock up to the right edge in anticipation of caching more
       * glyphs, or just enough for the current glyph?
       */
      if (lock_more)
         data->lock_rect.w = al_get_bitmap_width(page) - data->lock_rect.x;
      else
         data->lock_rect.w = glyph->region.w;

      data->page_lr = al_lock_bitmap_region(page,
         data->lock_rect.x, data->lock_rect.y,
         data->lock_rect.w, data->lock_rect.h,
         ALLEGRO_PIXEL_FORMAT_ABGR_8888_LE, ALLEGRO_LOCK_WRITEONLY);
   }

   ASSERT(data->page_lr);

   /* Copy a displaced pointer for the glyph. */
   return (unsigned char *)data->page_lr->data
      + (glyph->region.y - data->lock_rect.y) * data->page_lr->pitch
      + (glyph->region.x - data->lock_rect.x) * sizeof(int32_t);
}


static void copy_glyph_mono(ALLEGRO_TTF_FONT_DATA *font_data, FT_Face face,
   unsigned char *glyph_data)
{
   int pitch = font_data->page_lr->pitch;
   int x, y;

   for (y = 0; y < face->glyph->bitmap.rows; y++) {
      unsigned char const *ptr = face->glyph->bitmap.buffer + face->glyph->bitmap.pitch * y;
      unsigned char *dptr = glyph_data + pitch * y;
      int bit = 0;

      if (font_data->no_premultiply_alpha) {
         for (x = 0; x < face->glyph->bitmap.width; x++) {
            unsigned char set = ((*ptr >> (7-bit)) & 1) ? 255 : 0;
            *dptr++ = 255;
            *dptr++ = 255;
            *dptr++ = 255;
            *dptr++ = set;
            bit = (bit + 1) & 7;
            if (bit == 0) {
               ptr++;
            }
         }
      }
      else {
         for (x = 0; x < face->glyph->bitmap.width; x++) {
            unsigned char set = ((*ptr >> (7-bit)) & 1) ? 255 : 0;
            *dptr++ = set;
            *dptr++ = set;
            *dptr++ = set;
            *dptr++ = set;
            bit = (bit + 1) & 7;
            if (bit == 0) {
               ptr++;
            }
         }
      }
   }
}


static void copy_glyph_color(ALLEGRO_TTF_FONT_DATA *font_data, FT_Face face,
   unsigned char *glyph_data)
{
   int pitch = font_data->page_lr->pitch;
   int x, y;

   for (y = 0; y < face->glyph->bitmap.rows; y++) {
      unsigned char const *ptr = face->glyph->bitmap.buffer + face->glyph->bitmap.pitch * y;
      unsigned char *dptr = glyph_data + pitch * y;

      if (font_data->no_premultiply_alpha) {
         for (x = 0; x < face->glyph->bitmap.width; x++) {
            unsigned char c = *ptr;
            *dptr++ = 255;
            *dptr++ = 255;
            *dptr++ = 255;
            *dptr++ = c;
            ptr++;
         }
      }
      else {
         for (x = 0; x < face->glyph->bitmap.width; x++) {
            unsigned char c = *ptr;
            *dptr++ = c;
            *dptr++ = c;
            *dptr++ = c;
            *dptr++ = c;
            ptr++;
         }
      }
   }
}


/* NOTE: this function may disable the bitmap hold drawing state
 * and leave the current page bitmap locked.
 */
static void cache_glyph(ALLEGRO_TTF_FONT_DATA *font_data, FT_Face face,
   int ft_index, ALLEGRO_TTF_GLYPH_DATA *glyph, bool lock_more)
{
    FT_Int32 ft_load_flags;
    FT_Error e;
    int w, h;
    unsigned char *glyph_data;

    if (glyph->page_bitmap || glyph->region.x < 0)
        return;

    // FIXME: make this a config setting? FT_LOAD_FORCE_AUTOHINT

    // FIXME: Investigate why some fonts don't work without the
    // NO_BITMAP flags. Supposedly using that flag makes small sizes
    // look bad so ideally we would not used it.
    ft_load_flags = FT_LOAD_RENDER | FT_LOAD_NO_BITMAP;
    if (font_data->flags & ALLEGRO_TTF_MONOCHROME)
       ft_load_flags |= FT_LOAD_TARGET_MONO;

    e = FT_Load_Glyph(face, ft_index, ft_load_flags);
    if (e) {
       ALLEGRO_WARN("Failed loading glyph %d from.\n", ft_index);
    }

    glyph->offset_x = face->glyph->bitmap_left;
    glyph->offset_y = (face->size->metrics.ascender >> 6) - face->glyph->bitmap_top;
    glyph->advance = face->glyph->advance.x >> 6;

    w = face->glyph->bitmap.width;
    h = face->glyph->bitmap.rows;

    if (w == 0 || h == 0) {
       /* Mark this glyph so we won't try to cache it next time. */
       glyph->region.x = -1;
       glyph->region.y = -1;
       return;
    }

    glyph_data = alloc_glyph_region(font_data, w, h, false, glyph, lock_more);

    #ifdef ALIGN_TO_4_PIXEL
    {
       /* clear the extra borders we added */
       int pitch = font_data->page_lr->pitch;
       int bw = glyph->region.w;
       int bh = glyph->region.h;
       int i;

       for (i = 0; i < (bh - h); i++) {
          memset(glyph_data + (bh - 1 - i) * pitch, 0, 4 * bw);
       }
       for (i = 0; i < bh; i++) {
          memset(glyph_data + i * pitch + 4 * w, 0, 4 * (bw - w));
       }
    }
    #endif

    if (font_data->flags & ALLEGRO_TTF_MONOCHROME)
       copy_glyph_mono(font_data, face, glyph_data);
    else
       copy_glyph_color(font_data, face, glyph_data);
}


static int get_kerning(ALLEGRO_TTF_FONT_DATA const *data, FT_Face face,
   int prev_ft_index, int ft_index)
{
   /* Do kerning? */
   if (!(data->flags & ALLEGRO_TTF_NO_KERNING) && prev_ft_index != -1) {
      FT_Vector delta;
      FT_Get_Kerning(face, prev_ft_index, ft_index,
         FT_KERNING_DEFAULT, &delta);
      return delta.x >> 6;
   }

   return 0;
}


static int render_glyph(ALLEGRO_FONT const *f,
   ALLEGRO_COLOR color, int prev_ft_index, int ft_index,
   float xpos, float ypos)
{
   ALLEGRO_TTF_FONT_DATA *data = f->data;
   FT_Face face = data->face;
   ALLEGRO_TTF_GLYPH_DATA *glyph = get_glyph(data, ft_index);
   int advance = 0;

   /* We don't try to cache all glyphs in a pre-pass before drawing them.
    * While that would indeed save us making separate texture uploads, it
    * implies two passes over a string even in the common case when all glyphs
    * are already cached.  This turns out to have an measureable impact on
    * performance.
    */
   cache_glyph(data, face, ft_index, glyph, false);
   unlock_current_page(data);

   advance += get_kerning(data, face, prev_ft_index, ft_index);

   if (glyph->page_bitmap) {
      al_draw_tinted_bitmap_region(glyph->page_bitmap, color,
         glyph->region.x, glyph->region.y,
         glyph->region.w, glyph->region.h,
         xpos + glyph->offset_x + advance,
         ypos + glyph->offset_y, 0);
   }

   advance += glyph->advance;

   return advance;
}


static int ttf_font_height(ALLEGRO_FONT const *f)
{
   ASSERT(f);
   return f->height;
}


static int ttf_font_ascent(ALLEGRO_FONT const *f)
{
    ALLEGRO_TTF_FONT_DATA *data;
    FT_Face face;

    ASSERT(f);

    data = f->data;
    face = data->face;

    return face->size->metrics.ascender >> 6;
}


static int ttf_font_descent(ALLEGRO_FONT const *f)
{
    ALLEGRO_TTF_FONT_DATA *data;
    FT_Face face;

    ASSERT(f);

    data = f->data;
    face = data->face;

    return (-face->size->metrics.descender) >> 6;
}


static int ttf_render_char(ALLEGRO_FONT const *f, ALLEGRO_COLOR color,
   int ch, float xpos, float ypos)
{
   /* Unused method. */
   ASSERT(false);
   (void)f;
   (void)color;
   (void)ch;
   (void)xpos;
   (void)ypos;
   return 0;
}


static int ttf_char_length(ALLEGRO_FONT const *f, int ch)
{
   /* Unused method. */
   ASSERT(false);
   (void)f;
   (void)ch;
   return 0;
}


static int ttf_render(ALLEGRO_FONT const *f, ALLEGRO_COLOR color,
   const ALLEGRO_USTR *text, float x, float y)
{
   ALLEGRO_TTF_FONT_DATA *data = f->data;
   FT_Face face = data->face;
   int pos = 0;
   int advance = 0;
   int prev_ft_index = -1;
   int32_t ch;
   bool hold;

   hold = al_is_bitmap_drawing_held();
   al_hold_bitmap_drawing(true);

   while ((ch = al_ustr_get_next(text, &pos)) >= 0) {
      int ft_index = FT_Get_Char_Index(face, ch);
      advance += render_glyph(f, color, prev_ft_index, ft_index,
         x + advance, y);
      prev_ft_index = ft_index;
   }

   al_hold_bitmap_drawing(hold);

   return advance;
}


static int ttf_text_length(ALLEGRO_FONT const *f, const ALLEGRO_USTR *text)
{
   ALLEGRO_TTF_FONT_DATA *data = f->data;
   FT_Face face = data->face;
   int pos = 0;
   int prev_ft_index = -1;
   int x = 0;
   int32_t ch;
   bool hold;

   hold = al_is_bitmap_drawing_held();

   while ((ch = al_ustr_get_next(text, &pos)) >= 0) {
      int ft_index = FT_Get_Char_Index(face, ch);
      ALLEGRO_TTF_GLYPH_DATA *glyph = get_glyph(data, ft_index);

      cache_glyph(data, face, ft_index, glyph, true);

      x += get_kerning(data, face, prev_ft_index, ft_index);
      x += glyph->advance;

      prev_ft_index = ft_index;
   }

   al_hold_bitmap_drawing(hold);

   return x;
}


static void ttf_get_text_dimensions(ALLEGRO_FONT const *f,
   ALLEGRO_USTR const *text,
   int *bbx, int *bby, int *bbw, int *bbh)
{
   ALLEGRO_TTF_FONT_DATA *data = f->data;
   FT_Face face = data->face;
   int end;
   int pos = 0;
   int prev_ft_index = -1;
   bool first = true;
   int x = 0;
   int32_t ch;
   bool hold;

   hold = al_is_bitmap_drawing_held();
   end = al_ustr_size(text);
   *bbx = 0;

   while ((ch = al_ustr_get_next(text, &pos)) >= 0) {
      int ft_index = FT_Get_Char_Index(face, ch);
      ALLEGRO_TTF_GLYPH_DATA *glyph = get_glyph(data, ft_index);

      cache_glyph(data, face, ft_index, glyph, true);

      if (pos == end) {
         x += glyph->offset_x + glyph->region.w;
      }
      else {
         x += get_kerning(data, face, prev_ft_index, ft_index);
         x += glyph->advance;
      }

      if (first) {
         *bbx = glyph->offset_x;
         first = false;
      }

      prev_ft_index = ft_index;
   }

   *bby = 0; // FIXME
   *bbw = x - *bbx;
   *bbh = f->height; // FIXME, we want the bounding box!

   al_hold_bitmap_drawing(hold);
}


#ifdef DEBUG_CACHE
#include "allegro5/allegro_image.h"
static void debug_cache(ALLEGRO_FONT *f)
{
   ALLEGRO_TTF_FONT_DATA *data = f->data;
   _AL_VECTOR *v = &data->page_bitmaps;
   static int j = 0;
   int i;
   
   al_init_image_addon();
   
   for (i = 0; i < (int)_al_vector_size(v); i++) {
      ALLEGRO_BITMAP **bmp = _al_vector_ref(v, i);
      ALLEGRO_USTR *u = al_ustr_newf("font%d.png", j++);
      al_save_bitmap(al_cstr(u), *bmp);
      al_ustr_free(u);
   }
}
#endif


static void ttf_destroy(ALLEGRO_FONT *f)
{
   ALLEGRO_TTF_FONT_DATA *data = f->data;
   int i;

   unlock_current_page(data);

#ifdef DEBUG_CACHE
   debug_cache(f);
#endif

   FT_Done_Face(data->face);
   for (i = _al_vector_size(&data->glyph_ranges) - 1; i >= 0; i--) {
      ALLEGRO_TTF_GLYPH_RANGE *range = _al_vector_ref(&data->glyph_ranges, i);
      al_free(range->glyphs);
   }
   _al_vector_free(&data->glyph_ranges);
   for (i = _al_vector_size(&data->page_bitmaps) - 1; i >= 0; i--) {
      ALLEGRO_BITMAP **bmp = _al_vector_ref(&data->page_bitmaps, i);
      al_destroy_bitmap(*bmp);
   }
   _al_vector_free(&data->page_bitmaps);
   al_free(data);
   al_free(f);
}


static unsigned long ftread(FT_Stream stream, unsigned long offset,
    unsigned char *buffer, unsigned long count)
{
    ALLEGRO_TTF_FONT_DATA *data = stream->pathname.pointer;
    unsigned long bytes;

    if (count == 0)
       return 0;

    if (offset != data->offset)
       al_fseek(data->file, data->base_offset + offset, ALLEGRO_SEEK_SET);
    bytes = al_fread(data->file, buffer, count);
    data->offset = offset + bytes;
    return bytes;
}


static void ftclose(FT_Stream  stream)
{
    ALLEGRO_TTF_FONT_DATA *data = stream->pathname.pointer;
    al_fclose(data->file);
    data->file = NULL;
}


/* Function: al_load_ttf_font_f
 */
ALLEGRO_FONT *al_load_ttf_font_f(ALLEGRO_FILE *file,
    char const *filename, int size, int flags)
{
    return al_load_ttf_font_stretch_f(file, filename, 0, size, flags);
}


/* Function: al_load_ttf_font_stretch_f
 */
ALLEGRO_FONT *al_load_ttf_font_stretch_f(ALLEGRO_FILE *file,
    char const *filename, int w, int h, int flags)
{
    FT_Face face;
    ALLEGRO_TTF_FONT_DATA *data;
    ALLEGRO_FONT *f;
    ALLEGRO_PATH *path;
    FT_Open_Args args;

    data = al_malloc(sizeof *data);
    memset(data, 0, sizeof *data);
    data->stream.read = ftread;
    data->stream.close = ftclose;
    data->stream.pathname.pointer = data;
    data->base_offset = al_ftell(file);
    data->stream.size = al_fsize(file);
    data->file = file;

    memset(&args, 0, sizeof args);
    args.flags = FT_OPEN_STREAM;
    args.stream = &data->stream;

    if (FT_Open_Face(ft, &args, 0, &face) != 0) {
        ALLEGRO_DEBUG("Reading %s failed.\n", filename);
        // Note: Freetype already closed the file for us.
        al_free(data);
        return NULL;
    }

    // FIXME: The below doesn't use Allegro's streaming.
    /* Small hack for Type1 fonts which store kerning information in
     * a separate file - and we try to guess the name of that file.
     */
    path = al_create_path(filename);
    if (!strcmp(al_get_path_extension(path), ".pfa")) {
        const char *helper;
        ALLEGRO_DEBUG("Type1 font assumed for %s.\n", filename);

        al_set_path_extension(path, ".afm");
        helper = al_path_cstr(path, '/');
        FT_Attach_File(face, helper);
        ALLEGRO_DEBUG("Guessed afm file %s.\n", helper);

        al_set_path_extension(path, ".tfm");
        helper = al_path_cstr(path, '/');
        FT_Attach_File(face, helper);
        ALLEGRO_DEBUG("Guessed tfm file %s.\n", helper);
    }
    al_destroy_path(path);

    if (h > 0) {
       FT_Set_Pixel_Sizes(face, w, h);
    }
    else {
       /* Set the "real dimension" of the font to be the passed size,
        * in pixels.
        */
       FT_Size_RequestRec req;
       req.type = FT_SIZE_REQUEST_TYPE_REAL_DIM;
       req.width = (-w) << 6;
       req.height = (-h) << 6;
       req.horiResolution = 0;
       req.vertResolution = 0;
       FT_Request_Size(face, &req);
    }

    ALLEGRO_DEBUG("Font %s loaded with pixel size %d x %d.\n", filename,
        w, h);
    ALLEGRO_DEBUG("    ascent=%.1f, descent=%.1f, height=%.1f\n",
        face->size->metrics.ascender / 64.0,
        face->size->metrics.descender / 64.0,
        face->size->metrics.height / 64.0);

    data->face = face;
    data->flags = flags;
    data->no_premultiply_alpha =
       (al_get_new_bitmap_flags() & ALLEGRO_NO_PREMULTIPLIED_ALPHA);

    _al_vector_init(&data->glyph_ranges, sizeof(ALLEGRO_TTF_GLYPH_RANGE));
    _al_vector_init(&data->page_bitmaps, sizeof(ALLEGRO_BITMAP*));

    f = al_malloc(sizeof *f);
    f->height = face->size->metrics.height >> 6;
    f->vtable = &vt;
    f->data = data;

    _al_register_destructor(_al_dtor_list, f,
       (void (*)(void *))al_destroy_font);

    return f;
}


/* Function: al_load_ttf_font
 */
ALLEGRO_FONT *al_load_ttf_font(char const *filename, int size, int flags)
{
   return al_load_ttf_font_stretch(filename, 0, size, flags);
}


/* Function: al_load_ttf_font_stretch
 */
ALLEGRO_FONT *al_load_ttf_font_stretch(char const *filename, int w, int h,
   int flags)
{
   ALLEGRO_FILE *f;
   ALLEGRO_FONT *font;
   ASSERT(filename);

   f = al_fopen(filename, "rb");
   if (!f)
      return NULL;

   /* The file handle is owned by the function and the file is usually only
    * closed when the font is destroyed, in case Freetype has to load data
    * at a later time.
    */
   font = al_load_ttf_font_stretch_f(f, filename, w, h, flags);

   return font;
}


/* Function: al_init_ttf_addon
 */
bool al_init_ttf_addon(void)
{
   if (inited) return false;
   inited = true;

   FT_Init_FreeType(&ft);
   vt.font_height = ttf_font_height;
   vt.font_ascent = ttf_font_ascent;
   vt.font_descent = ttf_font_descent;
   vt.char_length = ttf_char_length;
   vt.text_length = ttf_text_length;
   vt.render_char = ttf_render_char;
   vt.render = ttf_render;
   vt.destroy = ttf_destroy;
   vt.get_text_dimensions = ttf_get_text_dimensions;
    
   al_register_font_loader(".ttf", al_load_ttf_font);
   /* Can't fail right now - in the future we might dynamically load
    * the FreeType DLL here and/or initialize FreeType (which both
    * could fail and would cause a false return).
    */
   return true;
}


/* Function: al_shutdown_ttf_addon
 */
void al_shutdown_ttf_addon(void)
{
   if (!inited) return;
   inited = false;
   
   al_register_font_loader(".ttf", NULL);
   
   FT_Done_FreeType(ft);
}


/* Function: al_get_allegro_ttf_version
 */
uint32_t al_get_allegro_ttf_version(void)
{
   return ALLEGRO_VERSION_INT;
}

/* vim: set sts=3 sw=3 et: */