/******************************************************************************
 * @file    renderer_draw.cpp
 * @brief   Renderer HUD/overlay draw pass (Renderer::Draw) and its private
 *          font/text helpers + cursor unproject. Split from renderer.cpp.
 *****************************************************************************/
#include "renderer_internal.h"
#include "graph_overlay.h"
#include "object_lightmap.h"
#include "tex_writer.h"
#include "../runtime/map_computer_projection.h"
#include "../runtime/pause_menu_layout.h"
#include "../runtime/pause_menu_font.h"
#include "../runtime/log_policy.h"
#include "../utils.h"
#include <vector>
#include <unordered_map>
#include <limits>
#include <cctype>

struct HudSprite {
  std::vector<GLuint> tex_ids;   // one texture per sprite frame (frame 0 for single-frame sprites)
  int width = 0;
  int height = 0;
  int frame_count = 0;
  bool valid = false;
};

static std::unordered_map<std::string, HudSprite> g_hudSprites;

static HudSprite GetOrLoadHudSprite(const std::string& spr_name) {
  auto it = g_hudSprites.find(spr_name);
  if (it != g_hudSprites.end()) return it->second;

  std::vector<std::string> paths = {
    Utils::GetIGIRootPath() + "\\screens\\game\\status\\STATUS_unpacked\\" + spr_name,
    "screens\\game\\status\\STATUS_unpacked\\" + spr_name,
    Utils::GetExeDirectory() + "\\screens\\game\\status\\STATUS_unpacked\\" + spr_name,
    "D:\\IGI1\\screens\\game\\status\\STATUS_unpacked\\" + spr_name
  };

  HudSprite res;
  for (const auto& p : paths) {
    if (File_Exists(p.c_str())) {
      TEXFile tex = TEX_Parse(p);
      if (tex.valid && !tex.images.empty()) {
        res.width       = (int)tex.images[0].width;
        res.height      = (int)tex.images[0].height;
        res.frame_count = (int)tex.images.size();
        for (size_t f = 0; f < tex.images.size(); ++f) {
          const TEXImage& img = tex.images[f];
          std::vector<uint8_t> rgba;
          if (img.mode == 2) {
            rgba.reserve(img.width * img.height * 4);
            for (size_t i = 0; i + 1 < img.pixels.size(); i += 2) {
              uint16_t p16 = img.pixels[i] | ((uint16_t)img.pixels[i + 1] << 8);
              uint8_t r = ((p16 >> 11) & 0x1F) << 3;
              uint8_t g = ((p16 >> 5)  & 0x3F) << 2;
              uint8_t b =  (p16        & 0x1F) << 3;
              uint8_t a = (r < 25 && g < 25 && b < 25) ? 0 : 255;
              rgba.push_back(r); rgba.push_back(g); rgba.push_back(b); rgba.push_back(a);
            }
          } else {
            rgba.resize(img.pixels.size());
            for (size_t i = 0; i + 3 < img.pixels.size(); i += 4) {
              rgba[i + 0] = img.pixels[i + 2]; // R <- B
              rgba[i + 1] = img.pixels[i + 1]; // G
              rgba[i + 2] = img.pixels[i + 0]; // B <- R
              rgba[i + 3] = img.pixels[i + 3]; // A
            }
          }
          pic_s pic;
          pic.width_  = (int)img.width;
          pic.height_ = (int)img.height;
          pic.pixels_ = rgba.data();
          GLuint t = GL_RegisterTexture(&pic, GL_CLAMP_TO_EDGE, GL_LINEAR, GL_LINEAR, false);
          if (t) res.tex_ids.push_back(t);
        }
        res.valid = !res.tex_ids.empty();
        break;
      }
    }
  }
  g_hudSprites[spr_name] = res;
  return res;
}

// Draw a whole selected frame of a HUD sprite. x,y = bottom-left corner in the
// ortho space (y=0 bottom), matching the existing crosshair/powerbar quads.
static void DrawHudFrame(const HudSprite& s, int frame, int x, int y, float r, float g, float b, float a) {
  if (!s.valid || s.tex_ids.empty()) return;
  if (frame < 0) frame = 0;
  if (frame >= (int)s.tex_ids.size()) frame = (int)s.tex_ids.size() - 1;
  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, s.tex_ids[frame]);
  glColor4f(r, g, b, a);
  glBegin(GL_QUADS);
  glTexCoord2f(0.0f, 1.0f); glVertex2i(x, y);
  glTexCoord2f(1.0f, 1.0f); glVertex2i(x + s.width, y);
  glTexCoord2f(1.0f, 0.0f); glVertex2i(x + s.width, y + s.height);
  glTexCoord2f(0.0f, 0.0f); glVertex2i(x, y + s.height);
  glEnd();
  glBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_TEXTURE_2D);
}

// Draw the bottom-filled portion of the powerbar frame 0. x,y = bottom-left
// corner; the top `1 - frac` of the sprite is cropped so it fills bottom-up.
static void DrawHudBarFill(const HudSprite& s, int x, int y, float frac, float r, float g, float b, float a) {
  if (!s.valid || s.tex_ids.empty()) return;
  frac = std::clamp(frac, 0.0f, 1.0f);
  int fill_h = (int)(s.height * frac);
  if (fill_h <= 0) return;
  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, s.tex_ids[0]);
  glColor4f(r, g, b, a);
  glBegin(GL_QUADS);
  glTexCoord2f(0.0f, 1.0f);           glVertex2i(x, y);
  glTexCoord2f(1.0f, 1.0f);           glVertex2i(x + s.width, y);
  glTexCoord2f(1.0f, 1.0f - frac);    glVertex2i(x + s.width, y + fill_h);
  glTexCoord2f(0.0f, 1.0f - frac);    glVertex2i(x, y + fill_h);
  glEnd();
  glBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_TEXTURE_2D);
}

static std::string GetWeaponSpriteName(const std::string& weapon_name) {
  std::string lower;
  for (char c : weapon_name) lower += (char)std::tolower((unsigned char)c);
  if (lower.find("m16") != std::string::npos) return "M16.spr";
  if (lower.find("ak") != std::string::npos) return "ak47.spr";
  if (lower.find("colt") != std::string::npos || lower.find("anaconda") != std::string::npos) return "colt.spr";
  if (lower.find("glock") != std::string::npos) return "glock.spr";
  if (lower.find("eagle") != std::string::npos) return "deserteagle.spr";
  if (lower.find("mp5") != std::string::npos) return "Mp5.spr";
  if (lower.find("uzi") != std::string::npos) return "uzi.spr";
  if (lower.find("spas") != std::string::npos || lower.find("spaz") != std::string::npos) return "spaz.spr";
  if (lower.find("jackhammer") != std::string::npos) return "jackhammer.spr";
  if (lower.find("saw") != std::string::npos || lower.find("minimi") != std::string::npos || lower.find("m249") != std::string::npos) return "SAW.spr";
  if (lower.find("dragunov") != std::string::npos || lower.find("druganov") != std::string::npos || lower.find("svd") != std::string::npos) return "druganov.spr";
  if (lower.find("law") != std::string::npos) return "LAW80.spr";
  if (lower.find("rpg") != std::string::npos || lower.find("rocket") != std::string::npos) return "rocket.spr";
  if (lower.find("knife") != std::string::npos) return "knife.spr";
  if (lower.find("flash") != std::string::npos) return "flashgrenade.spr";
  if (lower.find("grenade") != std::string::npos) return "spl_grenade.spr";
  if (lower.find("binocular") != std::string::npos) return "binoculars.spr";
  return "weapon.spr";
}

static FntFont g_editorFont;
static GLuint  g_editorFontTex = 0;
static bool    g_editorFontTried = false;

static FntFont g_editorSmFont;
static GLuint  g_editorSmFontTex = 0;
static bool    g_editorSmFontTried = false;

static GLuint  g_retailPauseFontTex = 0;
static const FntFont* g_retailPauseUploadedFont = nullptr;
static uint64_t g_retailPauseUploadedStamp = 0;

// Lazily load + upload the editor font atlas on first HUD draw.
static void EnsureEditorFont() {
  if (g_editorFontTried) {
    return;
  }
  g_editorFontTried = true;

  g_editorFont = FNT_Parse(Utils::GetExeDirectory() + "\\editor\\qed\\editor.fnt");
  if (!g_editorFont.valid) {
    return;
  }

  pic_s pic;
  pic.width_  = g_editorFont.texWidth;
  pic.height_ = g_editorFont.texHeight;
  pic.pixels_ = g_editorFont.rgba.data();
  // CLAMP + NEAREST so the small pixel font stays crisp (no blur/scaling).
  g_editorFontTex = GL_RegisterTexture(&pic, GL_CLAMP_TO_EDGE, GL_NEAREST, GL_NEAREST, false);
}

// Lazily load + upload the small editor font atlas (editorsm.fnt) for tooltips.
static void EnsureEditorSmFont() {
  if (g_editorSmFontTried) {
    return;
  }
  g_editorSmFontTried = true;

  g_editorSmFont = FNT_Parse("editor\\qed\\editorsm.fnt");
  if (!g_editorSmFont.valid) {
    return;
  }

  pic_s pic;
  pic.width_  = g_editorSmFont.texWidth;
  pic.height_ = g_editorSmFont.texHeight;
  pic.pixels_ = g_editorSmFont.rgba.data();
  g_editorSmFontTex = GL_RegisterTexture(&pic, GL_CLAMP_TO_EDGE, GL_NEAREST, GL_NEAREST, false);
}

static void EnsureRetailPauseFontTexture() {
  FntFont& font = GetRetailPauseFont();
  const uint64_t stamp = RetailPauseFontSourceStamp();
  if (!font.valid) return;
  if (g_retailPauseUploadedFont == &font && g_retailPauseUploadedStamp == stamp) return;
  pic_s pic;
  pic.width_ = font.texWidth;
  pic.height_ = font.texHeight;
  pic.pixels_ = font.rgba.data();
  g_retailPauseFontTex = GL_RegisterTexture(&pic, GL_CLAMP_TO_EDGE, GL_NEAREST,
                                             GL_NEAREST, false);
  if (g_retailPauseFontTex) {
    g_retailPauseUploadedFont = &font;
    g_retailPauseUploadedStamp = stamp;
  }
}

static void DrawRetailPauseText(int x, int y_gl, const char* str,
                                float r, float g, float b, float scale) {
  const FntFont& font = GetRetailPauseFont();
  if (!font.valid || !g_retailPauseFontTex) return;
  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, g_retailPauseFontTex);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glColor4f(r, g, b, 1.0f);
  // The public HUD y coordinate is a bitmap-font baseline, matching the GLUT
  // path. FNT glyphs use a line origin, so lift that origin before applying
  // the glyph's stored bearing; otherwise retail glyphs fall below their box.
  y_gl += 11;
  float pen_x = static_cast<float>(x);
  const float space = (font.defaultAdvance > 0 ? font.defaultAdvance : 4.0f) * scale;
  glBegin(GL_QUADS);
  for (const char* p = str; *p; ++p) {
    const auto it = font.glyphs.find(static_cast<unsigned char>(*p));
    if (it == font.glyphs.end()) { pen_x += space; continue; }
    const FntGlyph& glyph = it->second;
    const float x0 = pen_x + glyph.offsetX * scale;
    const float x1 = x0 + glyph.width * scale;
    const float y0 = y_gl - glyph.offsetY * scale;
    const float y1 = y0 - glyph.height * scale;
    glTexCoord2f(glyph.u0, glyph.v0); glVertex2f(x0, y0);
    glTexCoord2f(glyph.u1, glyph.v0); glVertex2f(x1, y0);
    glTexCoord2f(glyph.u1, glyph.v1); glVertex2f(x1, y1);
    glTexCoord2f(glyph.u0, glyph.v1); glVertex2f(x0, y1);
    pen_x += glyph.advance * scale;
  }
  glEnd();
  glBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_TEXTURE_2D);
  glDisable(GL_BLEND);
}

// Draw a string with the editor bitmap font in the HUD ortho space (y=0 bottom).
// y_gl is the gl-space y of the top of the first text line; glyphs extend
// downward from there. '\n' starts a new line.
static void DrawFontText(int x, int y_gl, const char* str, float r, float g, float b, float scale = 1.0f) {
  if (!g_editorFont.valid || !g_editorFontTex) {
    return;
  }

  // Keep the editor FNT path on the same baseline convention as GLUT and the
  // retail FNT path. This is especially important when the configured size is
  // larger than the original 12-point row.
  y_gl += 11;
  const float spaceAdvance = (g_editorFont.defaultAdvance > 0 ? g_editorFont.defaultAdvance : 4) * scale;

  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, g_editorFontTex);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glColor4f(r, g, b, 1.0f);

  // Float pen so fractional per-glyph scaling accumulates smoothly — every 1-pt
  // size step visibly changes width instead of rounding to the same integer.
  float pen_x = (float)x;
  float pen_y = (float)y_gl; // top of current line in gl space (y up)

  glBegin(GL_QUADS);
  for (const char* p = str; *p; ++p) {
    unsigned char c = (unsigned char)*p;
    if (c == '\n') {
      pen_x = (float)x;
      pen_y -= g_editorFont.lineHeight * scale;
      continue;
    }

    auto it = g_editorFont.glyphs.find((int)c);
    if (it == g_editorFont.glyphs.end()) {
      pen_x += spaceAdvance; // unknown char -> advance like a space
      continue;
    }

    const FntGlyph& gl = it->second;
    float x0 = pen_x + gl.offsetX * scale;
    float x1 = x0 + gl.width * scale;
    float yTop = pen_y - gl.offsetY * scale; // y up: line origin is above the glyph
    float yBot = yTop - gl.height * scale;

    // Atlas V grows downward; gl V grows upward -> top of glyph uses v0.
    glTexCoord2f(gl.u0, gl.v0); glVertex2f(x0, yTop);
    glTexCoord2f(gl.u1, gl.v0); glVertex2f(x1, yTop);
    glTexCoord2f(gl.u1, gl.v1); glVertex2f(x1, yBot);
    glTexCoord2f(gl.u0, gl.v1); glVertex2f(x0, yBot);

    pen_x += gl.advance * scale;
  }
  glEnd();

  glBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_TEXTURE_2D);
  glDisable(GL_BLEND);
}

// Draw a string with the small editor bitmap font (editorsm.fnt) for tooltips.
// scale mirrors DrawFontText so tooltip text honors the pause-menu Font Size
// instead of rendering the tiny native bitmap size.
static void DrawFontTextSm(int x, int y_gl, const char* str, float r, float g, float b, float scale = 1.0f) {
  if (!g_editorSmFont.valid || !g_editorSmFontTex) {
    return;
  }

  const float spaceAdvance = (g_editorSmFont.defaultAdvance > 0 ? g_editorSmFont.defaultAdvance : 4) * scale;

  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, g_editorSmFontTex);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glColor4f(r, g, b, 1.0f); // Use glColor4f to ensure alpha is 1

  float pen_x = (float)x;
  float pen_y = (float)y_gl;

  glBegin(GL_QUADS);
  for (const char* p = str; *p; ++p) {
    unsigned char c = (unsigned char)*p;
    if (c == '\n') {
      pen_x = (float)x;
      pen_y -= g_editorSmFont.lineHeight * scale;
      continue;
    }

    auto it = g_editorSmFont.glyphs.find((int)c);
    if (it == g_editorSmFont.glyphs.end()) {
      pen_x += spaceAdvance;
      continue;
    }

    const FntGlyph& gl = it->second;
    float x0 = pen_x + gl.offsetX * scale;
    float x1 = x0 + gl.width * scale;
    float yTop = pen_y - gl.offsetY * scale;
    float yBot = yTop - gl.height * scale;

    glTexCoord2f(gl.u0, gl.v0); glVertex2f(x0, yTop);
    glTexCoord2f(gl.u1, gl.v0); glVertex2f(x1, yTop);
    glTexCoord2f(gl.u1, gl.v1); glVertex2f(x1, yBot);
    glTexCoord2f(gl.u0, gl.v1); glVertex2f(x0, yBot);

    pen_x += gl.advance * scale;
  }
  glEnd();

  glBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_TEXTURE_2D);
  glDisable(GL_BLEND);
}

static bool UnprojectCursorToWorld(int mx, int my_topdown, int vpW, int vpH,
                                   const glm::mat4 &worldToClip,
                                   glm::vec3 &outWorld) {
  if (mx < 0 || my_topdown < 0 || mx >= vpW || my_topdown >= vpH)
    return false;
  int gly = vpH - 1 - my_topdown; // GL y is bottom-up
  float depth = 1.0f;
  glReadPixels(mx, gly, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
  if (depth >= 1.0f)
    return false; // nothing (cleared depth) -> sky
  glm::vec4 ndc(2.0f * mx / vpW - 1.0f, 2.0f * gly / vpH - 1.0f,
                2.0f * depth - 1.0f, 1.0f);
  glm::mat4 invPV = glm::inverse(worldToClip);
  glm::vec4 world = invPV * ndc;
  if (world.w == 0.0f)
    return false;
  outWorld = glm::vec3(world) / world.w;
  return true;
}


void Renderer::Draw(const draw_params_s &params,
                    const task_tree_view_params_s &task_tree_view) {
  static bool logged_params = false;
  if (!logged_params) {
    Logger::Get().Log(
        LogLevel::INFO,
        "[Renderer] Draw Params: draw_parts=" +
            std::to_string(params.draw_parts_) +
            " level_objects=" + (params.level_objects_ ? "VALID" : "NULL"));
    logged_params = true;
  }
  SetupUBOMats(*params.view_define_);

  // start draw
  glDepthMask(GL_TRUE); // insure clear depth buffer
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
  glViewport(0, 0, params.view_define_->viewport_width_,
             params.view_define_->viewport_height_);

  if (params.draw_parts_ & DRAW_SKYDOME) {
    skydome_.Draw(ubo_mats_, params.overlay_wireframe_);
  }

  if ((params.draw_parts_ & DRAW_FLAT_SKY_LAYER) &&
      params.flat_sky_layer_is_visible_) {
    flat_sky_layers_.Draw(ubo_mats_, params.overlay_wireframe_);
  }

  if (params.draw_parts_ & DRAW_TERRAIN) {
    terrain_.Draw(ubo_mats_, ubo_fog_, params.overlay_wireframe_,
                  params.draw_terrain_options_,
                  params.num_terrain_render_chunk_);
  }

  if ((params.draw_parts_ & (DRAW_OBJECTS | DRAW_BUILDINGS | DRAW_PROPS)) &&
      params.level_objects_) {
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(glm::value_ptr(mat_proj_));
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(glm::value_ptr(mat_view_));
    objects_.Draw(ubo_mats_, params.overlay_wireframe_,
                  params.level_objects_->GetObjects(),
                  params.selected_object_index_, task_tree_view.hover_object_index_,
                  params.draw_parts_, params.view_define_->pos_,
                  params.show_magic_obj_spheres_, params.skip_static_draw_indices_);
    splines_.Draw(params.level_objects_->GetObjects(), ubo_mats_,
                  objects_.GetShaderProgram());

  }

  // RainEffect remains independent of editor draw filters, but precipitation
  // is suppressed when the camera is within a Building's transformed bounds.
  const bool cameraIsSheltered = params.level_objects_ &&
      objects_.IsCameraInsideBuildingBounds(params.level_objects_->GetObjects(),
                                             params.view_define_->pos_);
  rain_.Draw(ubo_mats_, params.view_define_->pos_, cameraIsSheltered);

  // 3D navigation-graph pass: solid boxes + edges, depth-tested, before the HUD.
  if (graph_overlay_visible_)
    DrawGraphNodes3D(params);

  // 2D HUD overlay — always active so tooltip/pause/debug show even when TreeView is hidden
  {
    glUseProgram(0);      // Disable any active shaders for fixed-function HUD
    glBindVertexArray(0); // UNBIND VAO to prevent state leak
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    for (int i = 0; i < 4; ++i) glDisableVertexAttribArray(i);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, 0); // UNBIND UBO
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
    GL_CHECK_ERROR;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, params.view_define_->viewport_width_, 0,
            params.view_define_->viewport_height_, -1, 1);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    const bool useEditorFont = Config::Get().useEditorFont;
    if (useEditorFont) {
      EnsureEditorFont();
      EnsureEditorSmFont();
    } else {
      EnsureRetailPauseFontTexture();
    }

    // UI font scale derived from the configured size so the task tree, picker,
    // and property editor (all routed through draw_text) honor the Font Size
    // chosen in the pause menu. 12 is the baseline; 10/18 scale the editor
    // bitmap glyphs and pick the matching GLUT bitmap for the fallback path.
    const int uiFontSize = Config::Get().systemFontSize;
    const PauseMenuFontMetrics uiFontMetrics =
        GetPauseMenuFontMetrics(useEditorFont, uiFontSize);
    // Continuous scale: 12 is the baseline; size changes in 1-pt steps. The editor
    // bitmap font scales smoothly; the GLUT fallback picks the nearest bitmap.
    const float uiFontScale = uiFontMetrics.scale;
    void *uiGlutFont = (uiFontSize <= 11) ? GLUT_BITMAP_HELVETICA_10
                     : (uiFontSize >= 15) ? GLUT_BITMAP_HELVETICA_18
                                          : GLUT_BITMAP_HELVETICA_12;
    const int uiLineH = uiFontMetrics.lineHeight;

    // Pixel width of the first `count` chars of `str` in the active HUD font.
    // Mirrors DrawFontText's per-glyph advance exactly so callers (e.g. the text
    // caret) land on the true character boundary instead of a fixed-width guess.
    auto measure_text_width = [&](const char *str, int count) -> int {
      if (count <= 0) return 0;
      if (useEditorFont && g_editorFont.valid && g_editorFontTex) {
        const int spaceAdvance = g_editorFont.defaultAdvance > 0
            ? g_editorFont.defaultAdvance : 4;
        float w = 0.0f;
        for (int i = 0; i < count && str[i]; ++i) {
          auto it = g_editorFont.glyphs.find((int)(unsigned char)str[i]);
          w += static_cast<float>(it != g_editorFont.glyphs.end()
              ? it->second.advance : spaceAdvance) * uiFontScale;
        }
        return static_cast<int>(w);
      }
      if (!useEditorFont && GetRetailPauseFont().valid && g_retailPauseFontTex) {
        const FntFont& font = GetRetailPauseFont();
        const int spaceAdvance = font.defaultAdvance > 0 ? font.defaultAdvance : 4;
        float w = 0.0f;
        for (int i = 0; i < count && str[i]; ++i) {
          auto it = font.glyphs.find((int)(unsigned char)str[i]);
          w += static_cast<float>(it != font.glyphs.end()
              ? it->second.advance : spaceAdvance) * uiFontScale;
        }
        return static_cast<int>(w);
      }
      int w = 0;
      for (int i = 0; i < count && str[i]; ++i)
        w += glutBitmapWidth(uiGlutFont, (unsigned char)str[i]);
      return w;
    };

    auto draw_text = [&](int x, int y, const char *str, float r, float g,
                         float b) {
      if (useEditorFont && g_editorFont.valid && g_editorFontTex) {
        // Editor bitmap font path. y is top-down; convert each line's top to
        // gl space (y=0 bottom) and let glyphs extend downward.
        std::stringstream ss(str);
        std::string line;
        int line_y = y;
        while (std::getline(ss, line)) {
          int y_gl = params.view_define_->viewport_height_ - line_y;
          // 1px black shadow for readability, then the requested color.
          DrawFontText(x + 1, y_gl - 1, line.c_str(), 0.0f, 0.0f, 0.0f, uiFontScale);
          DrawFontText(x, y_gl, line.c_str(), r, g, b, uiFontScale);
          line_y += uiLineH; // Vertical spacing
        }
        return;
      }

      if (!useEditorFont && GetRetailPauseFont().valid && g_retailPauseFontTex) {
        std::stringstream ss(str);
        std::string line;
        int line_y = y;
        while (std::getline(ss, line)) {
          const int y_gl = params.view_define_->viewport_height_ - line_y;
          DrawRetailPauseText(x + 1, y_gl - 1, line.c_str(), 0.0f, 0.0f, 0.0f,
                              uiFontScale);
          DrawRetailPauseText(x, y_gl, line.c_str(), r, g, b, uiFontScale);
          line_y += uiLineH;
        }
        return;
      }

      // Fallback: GLUT bitmap font (size chosen from config).
      std::stringstream ss(str);
      std::string line;
      int line_y = y;
      while (std::getline(ss, line)) {
        // Draw shadow
        glColor3f(0.0f, 0.0f, 0.0f);
        glRasterPos2i(x + 1,
                      params.view_define_->viewport_height_ - line_y - 1);
        for (char c : line)
          glutBitmapCharacter(uiGlutFont, c);

        // Draw main text
        glColor3f(r, g, b);
        glRasterPos2i(x, params.view_define_->viewport_height_ - line_y);
        for (char c : line)
          glutBitmapCharacter(uiGlutFont, c);

        line_y += uiLineH; // Vertical spacing
      }
    };

    auto draw_text_mono = [&](int x, int y, const char *str, float r, float g,
                              float b) {
      std::stringstream ss(str);
      std::string line;
      int line_y = y;
      while (std::getline(ss, line)) {
        // Draw shadow
        glColor3f(0.0f, 0.0f, 0.0f);
        glRasterPos2i(x + 1,
                      params.view_define_->viewport_height_ - line_y - 1);
        for (char c : line)
          glutBitmapCharacter(GLUT_BITMAP_9_BY_15, c);

        // Draw main text
        glColor3f(r, g, b);
        glRasterPos2i(x, params.view_define_->viewport_height_ - line_y);
        for (char c : line)
          glutBitmapCharacter(GLUT_BITMAP_9_BY_15, c);

        line_y += 18; // Vertical spacing for 9x15
      }
    };

    // draw_text_sm: uses editorsm.fnt bitmap font for tooltip text, scaled by
    // the pause-menu Font Size like every other UI text (12 is the baseline).
    const float smScale = uiFontScale;
    const int smLineH = g_editorSmFont.valid && g_editorSmFont.lineHeight > 0
        ? (int)(g_editorSmFont.lineHeight * smScale) + 2 : uiLineH;
    auto draw_text_sm = [&](int x, int y, const char *str, float r, float g,
                            float b) {
      if (useEditorFont && g_editorSmFont.valid && g_editorSmFontTex) {
        std::stringstream ss(str);
        std::string line;
        int line_y = y;
        while (std::getline(ss, line)) {
          int y_gl = params.view_define_->viewport_height_ - line_y;
          DrawFontTextSm(x + 1, y_gl - 1, line.c_str(), 0.0f, 0.0f, 0.0f, smScale);
          DrawFontTextSm(x, y_gl, line.c_str(), r, g, b, smScale);
          line_y += smLineH;
        }
        return;
      }
      // Fallback: GLUT bitmap font, sized by the configured UI font size.
      std::stringstream ss(str);
      std::string line;
      int line_y = y;
      while (std::getline(ss, line)) {
        glColor3f(0.0f, 0.0f, 0.0f);
        glRasterPos2i(x + 1, params.view_define_->viewport_height_ - line_y - 1);
        for (char c : line) glutBitmapCharacter(uiGlutFont, c);
        glColor3f(r, g, b);
        glRasterPos2i(x, params.view_define_->viewport_height_ - line_y);
        for (char c : line) glutBitmapCharacter(uiGlutFont, c);
        line_y += uiLineH;
      }
    };

    // draw_text_sys: always uses GLUT system font (for Escape menu, Help, Debug).
    // Font size is chosen via Config::Get().systemFontSize (10, 12, or 18).
    const int sysFontSize = Config::Get().systemFontSize;
    void* sysFontPtr = (sysFontSize <= 11) ? GLUT_BITMAP_HELVETICA_10
                     : (sysFontSize >= 15) ? GLUT_BITMAP_HELVETICA_18
                                           : GLUT_BITMAP_HELVETICA_12;
    const int sysLineH = (sysFontSize <= 11) ? 13 : (sysFontSize >= 15) ? 22 : 15;
    auto draw_text_sys = [&](int x, int y, const char *str, float r, float g,
                             float b) {
      std::stringstream ss(str);
      std::string line;
      int line_y = y;
      while (std::getline(ss, line)) {
        glColor3f(0.0f, 0.0f, 0.0f);
        glRasterPos2i(x + 1, params.view_define_->viewport_height_ - line_y - 1);
        for (char c : line) glutBitmapCharacter(sysFontPtr, c);
        glColor3f(r, g, b);
        glRasterPos2i(x, params.view_define_->viewport_height_ - line_y);
        for (char c : line) glutBitmapCharacter(sysFontPtr, c);
        line_y += sysLineH;
      }
    };

    // Helper to convert KeyBinding to display string
    auto keybinding_to_string = [&](const KeyBinding &kb) -> std::string {
      std::string result;
      if (kb.ctrl)
        result += "CTRL+";
      if (kb.shift)
        result += "SHIFT+";
      if (kb.alt)
        result += "ALT+";

      // Convert vkCode to key name
      if (kb.vkCode >= VK_F1 && kb.vkCode <= VK_F12) {
        result += "F" + std::to_string(kb.vkCode - VK_F1 + 1);
      } else {
        // For regular keys, use the character
        char key = MapVirtualKeyA(kb.vkCode, MAPVK_VK_TO_CHAR) & 0xFF;
        if (key) {
          result += std::string(1, toupper(key));
        } else {
          result += "?";
        }
      }
      return result;
    };

    // Get font color from config
    ConfigData &cfg = Config::Get();
    float font_r = cfg.fontColorR / 255.0f;
    float font_g = cfg.fontColorG / 255.0f;
    float font_b = cfg.fontColorB / 255.0f;

    int line_y = 30;

    // --- TreeView HUD Implementation (only when TaskTree is visible and not in game mode) ---
    if (task_tree_view.show_hud_ && !task_tree_view.in_game_mode_) {
    if (task_tree_view.level_objects_ && !task_tree_view.prop_editor_open_ && !task_tree_view.task_picker_open_) {
      const auto &objects = task_tree_view.level_objects_->GetObjects();
      int tree_x = 20;
      int tree_y = 30; // Starting Y for tree
      int row_h = uiFontMetrics.rowHeight;
      int start_y = 30;
      int current_row = 0;
      int scroll_offset = task_tree_view.tree_scroll_offset;
      int viewport_h = params.view_define_->viewport_height_;

      // Recursive helper to draw tree nodes
      current_row = 0;
      std::function<void(int, int)> draw_node = [&](int idx, int depth) {
        if (idx < 0 || idx >= (int)objects.size())
          return;
        const auto &obj = objects[idx];
        if (obj.deleted)
          return;

        int x = tree_x + (depth * 18);
        int y =
            start_y + (current_row - task_tree_view.tree_scroll_offset) * row_h;
        current_row++;

        if (y >= start_y && y < params.view_define_->viewport_height_ - 50) {
          // Highlight if selected or hovered
          if (idx == task_tree_view.selected_object_index_) {
            glEnable(GL_BLEND);
            glColor4f(0.0f, 0.5f, 0.0f, 0.4f);
            glBegin(GL_QUADS);
            glVertex2i(tree_x - 5,
                       params.view_define_->viewport_height_ - (y - 2));
            glVertex2i(tree_x + 300,
                       params.view_define_->viewport_height_ - (y - 2));
            glVertex2i(tree_x + 300,
                       params.view_define_->viewport_height_ - (y + row_h - 2));
            glVertex2i(tree_x - 5,
                       params.view_define_->viewport_height_ - (y + row_h - 2));
            glEnd();
          } else if (idx == task_tree_view.hover_tree_index_) {
            glEnable(GL_BLEND);
            glColor4f(0.3f, 0.3f, 0.3f, 0.3f);
            glBegin(GL_QUADS);
            glVertex2i(tree_x - 5,
                       params.view_define_->viewport_height_ - (y - 2));
            glVertex2i(tree_x + 300,
                       params.view_define_->viewport_height_ - (y - 2));
            glVertex2i(tree_x + 300,
                       params.view_define_->viewport_height_ - (y + row_h - 2));
            glVertex2i(tree_x - 5,
                       params.view_define_->viewport_height_ - (y + row_h - 2));
            glEnd();
          }

          // Draw Hierarchy Line (dotted vertical)
          if (depth > 0) {
            glLineStipple(1, 0xAAAA);
            glEnable(GL_LINE_STIPPLE);
            glColor3f(0.5f, 0.5f, 0.5f);
            glBegin(GL_LINES);
            // Vertical line from parent down to this node
            glVertex2i(x - 9, viewport_h - (y - row_h / 2));
            glVertex2i(x - 9, viewport_h - (y + row_h / 2));
            // Horizontal line to the node
            glVertex2i(x - 9, viewport_h - (y + row_h / 2));
            glVertex2i(x - 2, viewport_h - (y + row_h / 2));
            glEnd();
            glDisable(GL_LINE_STIPPLE);
          }

          // Expansion Toggle [+] or [-] box
          if (obj.isContainer && !obj.childrenIndices.empty()) {
            glColor3f(1.0f, 1.0f, 1.0f);
            glBegin(GL_LINE_LOOP);
            glVertex2i(x - 14, viewport_h - (y + 4));
            glVertex2i(x - 6, viewport_h - (y + 4));
            glVertex2i(x - 6, viewport_h - (y + 12));
            glVertex2i(x - 14, viewport_h - (y + 12));
            glEnd();
            // Draw minus
            glBegin(GL_LINES);
            glVertex2i(x - 12, viewport_h - (y + 8));
            glVertex2i(x - 8, viewport_h - (y + 8));
            // Draw plus vertical
            if (!obj.expanded) {
              glVertex2i(x - 10, viewport_h - (y + 6));
              glVertex2i(x - 10, viewport_h - (y + 10));
            }
            glEnd();
          } else if (depth > 0) {
            // Just a horizontal dash if no children
            glColor3f(0.5f, 0.5f, 0.5f);
            glBegin(GL_LINES);
            glVertex2i(x - 14, viewport_h - (y + 8));
            glVertex2i(x - 6, viewport_h - (y + 8));
            glEnd();
          }

          // Draw Yellow Folder Icon
          glColor3f(1.0f, 0.9f, 0.2f);
          glBegin(GL_QUADS);
          glVertex2i(x, viewport_h - (y + 2));
          glVertex2i(x + 12, viewport_h - (y + 2));
          glVertex2i(x + 12, viewport_h - (y + 12));
          glVertex2i(x, viewport_h - (y + 12));
          glEnd();
          // Folder tab
          glBegin(GL_QUADS);
          glVertex2i(x, viewport_h - y);
          glVertex2i(x + 5, viewport_h - y);
          glVertex2i(x + 5, viewport_h - (y + 2));
          glVertex2i(x, viewport_h - (y + 2));
          glEnd();
          // Folder outline
          glColor3f(0.0f, 0.0f, 0.0f);
          glBegin(GL_LINE_LOOP);
          glVertex2i(x, viewport_h - (y + 2));
          glVertex2i(x + 12, viewport_h - (y + 2));
          glVertex2i(x + 12, viewport_h - (y + 12));
          glVertex2i(x, viewport_h - (y + 12));
          glEnd();

          // Format label: Type (ID, "Name")
          std::string label = obj.type;
          if (!obj.taskId.empty() && obj.taskId != "-1") {
            label += " (" + obj.taskId;
            if (!obj.name.empty())
              label += ", \"" + obj.name + "\"";
            label += ")";
          } else if (!obj.name.empty()) {
            label += " (\"" + obj.name + "\")";
          }

          float tr = font_r, tg = font_g, tb = font_b;
          if (idx == task_tree_view.selected_object_index_) {
            tr = 1.0f;
            tg = 1.0f;
            tb = 0.0f;
          } // Selected = Yellow
          else if (idx == task_tree_view.hover_object_index_) {
            tr = 0.5f;
            tg = 0.8f;
            tb = 1.0f;
          } // Hover = Blue

          // Note: We use y + 11 for draw_text so the baseline aligns correctly
          // with the hitbox
          draw_text(x + 16, y + 11, label.c_str(), tr, tg, tb);
        }

        if (obj.expanded) {
          for (int childIdx : obj.childrenIndices) {
            draw_node(childIdx, depth + 1);
          }
        }
      };

      // To keep the root clean, group all Task_DeclareParameters into a virtual
      // "Mission Declarations" folder
      std::vector<int> root_decls;
      std::vector<int> root_others;

      for (int i = 0; i < (int)objects.size(); ++i) {
        if (objects[i].parentIndex == -1 && !objects[i].deleted) {
          if (objects[i].type == "Task_DeclareParameters")
            root_decls.push_back(i);
          else
            root_others.push_back(i);
        }
      }

      if (!root_decls.empty()) {
        int y = start_y + (current_row - scroll_offset) * row_h;
        current_row++;
        if (y >= start_y && y < viewport_h - 50) {
          int vx = tree_x;
          // Draw folder icon
          glColor3f(1.0f, 0.9f, 0.2f);
          glBegin(GL_QUADS);
          glVertex2i(vx, viewport_h - (y + 2));
          glVertex2i(vx + 12, viewport_h - (y + 2));
          glVertex2i(vx + 12, viewport_h - (y + 12));
          glVertex2i(vx, viewport_h - (y + 12));
          glEnd();
          // Folder tab
          glBegin(GL_QUADS);
          glVertex2i(vx, viewport_h - y);
          glVertex2i(vx + 5, viewport_h - y);
          glVertex2i(vx + 5, viewport_h - (y + 2));
          glVertex2i(vx, viewport_h - (y + 2));
          glEnd();
          // Folder outline
          glColor3f(0.0f, 0.0f, 0.0f);
          glBegin(GL_LINE_LOOP);
          glVertex2i(vx, viewport_h - (y + 2));
          glVertex2i(vx + 12, viewport_h - (y + 2));
          glVertex2i(vx + 12, viewport_h - (y + 12));
          glVertex2i(vx, viewport_h - (y + 12));
          glEnd();

          // Draw toggle box
          glColor3f(1.0f, 1.0f, 1.0f);
          glBegin(GL_LINE_LOOP);
          glVertex2i(vx - 14, viewport_h - (y + 4));
          glVertex2i(vx - 6, viewport_h - (y + 4));
          glVertex2i(vx - 6, viewport_h - (y + 12));
          glVertex2i(vx - 14, viewport_h - (y + 12));
          glEnd();
          // Draw minus
          glBegin(GL_LINES);
          glVertex2i(vx - 12, viewport_h - (y + 8));
          glVertex2i(vx - 8, viewport_h - (y + 8));
          // Draw plus vertical
          if (!task_tree_view.tree_decl_expanded) {
            glVertex2i(vx - 10, viewport_h - (y + 6));
            glVertex2i(vx - 10, viewport_h - (y + 10));
          }
          glEnd();

          if (task_tree_view.selected_object_index_ == -2) {

            glEnable(GL_BLEND);

            glColor4f(0.0f, 0.5f, 0.0f, 0.4f);

            glBegin(GL_QUADS);

            glVertex2i(vx - 5, viewport_h - (y - 2));

            glVertex2i(vx + 300, viewport_h - (y - 2));

            glVertex2i(vx + 300, viewport_h - (y + row_h - 2));

            glVertex2i(vx - 5, viewport_h - (y + row_h - 2));

            glEnd();

          } else if (task_tree_view.hover_object_index_ == -2) {

            glEnable(GL_BLEND);

            glColor4f(0.3f, 0.3f, 0.3f, 0.3f);

            glBegin(GL_QUADS);

            glVertex2i(vx - 5, viewport_h - (y - 2));

            glVertex2i(vx + 300, viewport_h - (y - 2));

            glVertex2i(vx + 300, viewport_h - (y + row_h - 2));

            glVertex2i(vx - 5, viewport_h - (y + row_h - 2));

            glEnd();
          }

          float dtr = 0.7f, dtg = 0.7f, dtb = 0.7f;

          if (task_tree_view.selected_object_index_ == -2) {
            dtr = 1.0f;
            dtg = 1.0f;
            dtb = 0.0f;
          }

          else if (task_tree_view.hover_object_index_ == -2) {
            dtr = 0.5f;
            dtg = 0.8f;
            dtb = 1.0f;
          }

          draw_text(vx + 16, y + 11, "Mission Declarations", dtr, dtg, dtb);
        }
        if (task_tree_view.tree_decl_expanded) {
          for (int idx : root_decls)
            draw_node(idx, 1);
        }
      }

      for (int idx : root_others) {
        draw_node(idx, 0);
      }

      line_y = start_y + (current_row - scroll_offset) * row_h + 20;
    }
    } // end show_hud_ tree panel
    
    // Display object info at mouse position
    int info_object_index = task_tree_view.hover_object_index_;
    if (task_tree_view.in_game_mode_) info_object_index = -1;
    if (!task_tree_view.status_msg_.empty() &&
        task_tree_view.selected_object_index_ >= 0) {
      info_object_index = task_tree_view.selected_object_index_;
    }

    // Suppress tooltip when: in game mode, camera orbit mode, over TaskTree, or over property panel
    {
        bool over_tree  = task_tree_view.show_hud_ && task_tree_view.mouse_x_ < 350;
        bool over_panel = task_tree_view.prop_editor_open_ &&
                          task_tree_view.mouse_x_ < (PropPanel::kLeft + PropPanel::kWidth);
        if (task_tree_view.in_game_mode_ || over_tree || over_panel || task_tree_view.enable_camera_mode_) info_object_index = -1;
    }

    if (info_object_index >= 0 && task_tree_view.level_objects_) {
      const auto &objects = task_tree_view.level_objects_->GetObjects();
      if (info_object_index < (int)objects.size()) {
        const auto &obj = objects[info_object_index];

        // Skip labels for skipped models (Poles, Wires, etc.)
        if (Renderer_Objects::IsSkippedModelId(obj.modelId)) {
          info_object_index = -1; // Reset to hide label
        }
      }
    }

    int tooltip_x = task_tree_view.mouse_x_ + 20;  // right of cursor
    int tooltip_y = task_tree_view.mouse_y_;



    if (tooltip_x < 10)
      tooltip_x = 10;
    if (tooltip_x > params.view_define_->viewport_width_ - (int)(260 * smScale))
      tooltip_x = params.view_define_->viewport_width_ - (int)(260 * smScale);
    if (tooltip_y < 10)
      tooltip_y = 10;
    if (tooltip_y > params.view_define_->viewport_height_ - (int)(100 * smScale))
      tooltip_y = params.view_define_->viewport_height_ - (int)(100 * smScale);

    if (info_object_index >= 0 && task_tree_view.level_objects_) {
      const auto &objects = task_tree_view.level_objects_->GetObjects();
      if (info_object_index < (int)objects.size()) {
        const auto &obj = objects[info_object_index];

        char buf[512];
        // Look up model name from IGIModels.json; fall back to task note or type
        std::string display_name;
        if (task_tree_view.level_objects_) {
            display_name = task_tree_view.level_objects_->GetModelName(obj.modelId);
        }
        if (display_name.empty()) display_name = obj.type;
        if (display_name.empty()) display_name = obj.modelId;

        std::string task_id = obj.taskId.empty() ? "-1" : obj.taskId;

        int text_x = tooltip_x;
        int text_y = tooltip_y;

        bool isAI = !obj.aiId.empty() || obj.type.find("AITYPE") == 0 ||
                    obj.type == "HumanSoldier" || obj.type == "HumanAI" || obj.type == "HumanSoldierFemale" || obj.type == "HumanPlayer";
        if (isAI) {
          std::string aiName = obj.name.empty() ? "AI Soldier" : obj.name;
          snprintf(buf, sizeof(buf), "Name: %s (Type: %s)", aiName.c_str(), obj.type.c_str());
          draw_text_sm(text_x, text_y, buf, 0.0f, 0.8f, 1.0f); // Sky blue title
          text_y += smLineH;

          snprintf(buf, sizeof(buf), "Soldier ID: %s | AI ID: %s",
                   task_id.c_str(), obj.aiId.empty() ? "-1" : obj.aiId.c_str());
          draw_text_sm(text_x, text_y, buf, 1.0f, 1.0f, 1.0f);
          text_y += smLineH;

          // Read Team from argTokens via schema (more accurate than obj.team)
          int teamVal = obj.team;
          {
            const TaskSchema* sc = GetSchema(obj.type);
            if (sc) {
              for (auto& fd : *sc) {
                if (fd.name == "Team" && fd.argOffset < (int)obj.argTokens.size()) {
                  try { teamVal = std::stoi(obj.argTokens[fd.argOffset]); } catch (...) {}
                  break;
                }
              }
            }
          }
          std::string teamStr = (teamVal == 0) ? "Friendly" : "Enemy";
          float tr = (teamVal == 0) ? 0.2f : 1.0f;
          float tg = (teamVal == 0) ? 1.0f : 0.2f;
          float tb = 0.2f;
          snprintf(buf, sizeof(buf), "Team: %s (%d)", teamStr.c_str(), teamVal);
          draw_text_sm(text_x, text_y, buf, tr, tg, tb);
          text_y += smLineH;

          if (!obj.primaryWeapon.empty()) {
            snprintf(buf, sizeof(buf), "Pri: %s (%s Ammo)",
                     obj.primaryWeapon.c_str(), obj.primaryAmmo.c_str());
            draw_text_sm(text_x, text_y, buf, 0.9f, 0.9f, 0.9f);
            text_y += smLineH;
          }

          if (!obj.secondaryWeapon.empty()) {
            snprintf(buf, sizeof(buf), "Sec: %s (%s Ammo)",
                     obj.secondaryWeapon.c_str(), obj.secondaryAmmo.c_str());
            draw_text_sm(text_x, text_y, buf, 0.8f, 0.8f, 0.8f);
            text_y += smLineH;
          }

          if (!obj.graphName.empty() || !obj.graphId.empty()) {
            snprintf(buf, sizeof(buf), "Graph: %s (ID: %s)",
                     obj.graphName.c_str(), obj.graphId.c_str());
            draw_text_sm(text_x, text_y, buf, 0.9f, 0.6f, 0.9f);
            text_y += smLineH;
          }
        } else {
          snprintf(buf, sizeof(buf), "%s ID: %s", display_name.c_str(),
                   task_id.c_str());
          draw_text_sm(text_x, text_y, buf, 1.0f, 1.0f, 1.0f);
          text_y += smLineH;

          snprintf(buf, sizeof(buf), "%s", obj.modelId.c_str());
          draw_text_sm(text_x, text_y, buf, 0.0f, 1.0f, 0.0f);  // always green
          text_y += smLineH;
        }

        if (!task_tree_view.status_msg_.empty() &&
            info_object_index == task_tree_view.selected_object_index_) {
          draw_text_sm(text_x, text_y, task_tree_view.status_msg_.c_str(), 0.85f,
                    1.0f, 0.6f);
        }
      }
    } else if (!task_tree_view.in_game_mode_ && !task_tree_view.pause_mode_ && (!task_tree_view.show_hud_ || task_tree_view.mouse_x_ >= 350)) {
      int terrainId = -1;
      if (params.terrain_id_at_world_xy_) {
        // Same world->clip transform the 3D scene uses (proj * view * scale_down);
        // its inverse maps the depth-buffer hit back to full IGI world coords.
        glm::mat4 worldToClip =
            mat_proj_ * mat_view_ *
            glm::scale(glm::mat4(1.0f), glm::vec3(RENDERER_MODEL_SCALE_DOWN));
        glm::vec3 worldPt;
        if (UnprojectCursorToWorld(task_tree_view.mouse_x_, task_tree_view.mouse_y_,
                                   params.view_define_->viewport_width_,
                                   params.view_define_->viewport_height_,
                                   worldToClip, worldPt)) {
          terrainId = params.terrain_id_at_world_xy_((double)worldPt.x, (double)worldPt.y);
        }
      }
      char tbuf[96];
      if (terrainId >= 0) {
        snprintf(tbuf, sizeof(tbuf), "Terrain ID: %d", terrainId);
        draw_text_sm(tooltip_x, tooltip_y, tbuf, 1.0f, 1.0f, 1.0f);
      } else {
        draw_text_sm(tooltip_x, tooltip_y, "Terrain ID: -1", 1.0f, 1.0f, 1.0f);
      }
    }

    // Terrain-edit overlay: 3D brush-radius rings + brush name label.
    // Rendered independently of hover state so rings appear on right-click regardless
    // of whether an object is under the cursor or where the task tree panel is.
    if (!task_tree_view.in_game_mode_ && !task_tree_view.pause_mode_ && task_tree_view.terrain_edit_enabled_) {
      const int vpW = params.view_define_->viewport_width_;
      const int vpH = params.view_define_->viewport_height_;

      glm::mat4 worldToClip =
          mat_proj_ * mat_view_ *
          glm::scale(glm::mat4(1.0f), glm::vec3(RENDERER_MODEL_SCALE_DOWN));

      glm::vec3 centerWorld;
      bool haveCenter =
          params.terrain_z_at_world_xy_ &&
          UnprojectCursorToWorld(task_tree_view.mouse_x_, task_tree_view.mouse_y_,
                                 vpW, vpH, worldToClip, centerWorld);

      if (haveCenter) {
        const double cx = centerWorld.x;
        const double cy = centerWorld.y;
        float centerZ = centerWorld.z;
        params.terrain_z_at_world_xy_(cx, cy, centerZ);

        const int segments = 48;
        auto emit_ring = [&](double radius) {
          glBegin(GL_LINE_LOOP);
          for (int i = 0; i < segments; ++i) {
            double a = (double)i * 6.283185307 / (double)segments;
            double px = cx + radius * cos(a);
            double py = cy + radius * sin(a);
            float pz = centerZ;
            params.terrain_z_at_world_xy_(px, py, pz);
            glm::vec4 clip = worldToClip *
                glm::vec4((float)px, (float)py, pz, 1.0f);
            if (clip.w <= 0.0f) continue;
            float sx = (clip.x / clip.w * 0.5f + 0.5f) * vpW;
            float sy = (clip.y / clip.w * 0.5f + 0.5f) * vpH;
            glVertex2f(sx, sy);
          }
          glEnd();
        };

        glLineWidth(2.0f);
        glColor3f(1.0f, 0.5f, 0.0f);
        emit_ring(task_tree_view.terrain_brush_radius_);
        glColor4f(1.0f, 0.6f, 0.1f, 0.6f);
        emit_ring(task_tree_view.terrain_brush_radius_ * 0.5);
        glLineWidth(1.0f);
      }

      static const char* kBrushNames[4] = { "Raise", "Lower", "Soften", "Flatten" };
      int b = task_tree_view.terrain_brush_;
      if (b < 0) b = 0; if (b > 3) b = 3;
      draw_text_sm(tooltip_x, tooltip_y + 16, kBrushNames[b], 1.0f, 0.7f, 0.2f);
    }

    // Navigation-graph overlay (ShowGraphNodes): nodes/edges/labels + hover tooltip.
    if (graph_overlay_visible_) {
      DrawGraphOverlayInternal(params, draw_text_sm,
                               task_tree_view.mouse_x_, task_tree_view.mouse_y_);
      // Left-side properties panel for the selected node.
      if (graph_overlay_selected_ >= 0)
        DrawGraphNodePanel(params, draw_text_sm);
    }

    // On-screen terrain editor panel (bottom-right): 5 brush buttons + a 2x2 grid
    // of radius/strength settings buttons with value readouts. Drawn independently
    // of the hover-tooltip branch so it stays visible while the cursor is over an
    // object or the left tree panel.
    if (task_tree_view.terrain_edit_enabled_) {
      const int vw = params.view_define_->viewport_width_;
      const int vh = params.view_define_->viewport_height_;
      int active_idx = TerrainPalette::IndexForBrush(task_tree_view.terrain_brush_);
      for (int i = 0; i < TerrainPalette::kBrushCount; ++i) {
        int rx, ry_top, rw, rh;
        TerrainPalette::GetButtonRect(i, vw, vh, rx, ry_top, rw, rh);
        // Top-down rect -> GL bottom-up.
        float bx = (float)rx;
        float by = (float)(vh - (ry_top + rh));
        float bw = (float)rw, bh = (float)rh;
        bool active = (i == active_idx);

        // Fill.
        glEnable(GL_BLEND);
        if (active) glColor4f(0.95f, 0.55f, 0.10f, 0.85f);
        else        glColor4f(0.12f, 0.12f, 0.14f, 0.80f);
        glBegin(GL_QUADS);
        glVertex2f(bx, by);
        glVertex2f(bx + bw, by);
        glVertex2f(bx + bw, by + bh);
        glVertex2f(bx, by + bh);
        glEnd();

        // Border.
        if (active) glColor3f(1.0f, 0.85f, 0.3f);
        else        glColor3f(0.55f, 0.55f, 0.6f);
        glLineWidth(active ? 2.5f : 1.5f);
        glBegin(GL_LINE_LOOP);
        glVertex2f(bx, by);
        glVertex2f(bx + bw, by);
        glVertex2f(bx + bw, by + bh);
        glVertex2f(bx, by + bh);
        glEnd();

        // Centered line-art glyph.
        float gcx = bx + bw * 0.5f;
        float gcy = by + bh * 0.5f;
        float s = bw * 0.28f;
        glColor3f(active ? 0.1f : 0.9f, active ? 0.1f : 0.9f, active ? 0.1f : 0.95f);
        glLineWidth(2.0f);
        switch (i) {
        case 0: // Select / exit: arrow pointing up-left
          glBegin(GL_LINE_STRIP);
          glVertex2f(gcx + s, gcy - s);
          glVertex2f(gcx - s, gcy + s);
          glEnd();
          glBegin(GL_LINE_STRIP);
          glVertex2f(gcx - s, gcy + s * 0.1f);
          glVertex2f(gcx - s, gcy + s);
          glVertex2f(gcx - s * 0.1f, gcy + s);
          glEnd();
          break;
        case 1: // Raise: up triangle
          glBegin(GL_LINE_LOOP);
          glVertex2f(gcx, gcy + s);
          glVertex2f(gcx - s, gcy - s);
          glVertex2f(gcx + s, gcy - s);
          glEnd();
          break;
        case 2: // Lower: down triangle
          glBegin(GL_LINE_LOOP);
          glVertex2f(gcx, gcy - s);
          glVertex2f(gcx - s, gcy + s);
          glVertex2f(gcx + s, gcy + s);
          glEnd();
          break;
        case 3: // Soften: wavy line
          glBegin(GL_LINE_STRIP);
          for (int k = 0; k <= 12; ++k) {
            float t = (float)k / 12.0f;
            float px = gcx - s + t * 2.0f * s;
            float py = gcy + sinf(t * 6.283185f) * s * 0.5f;
            glVertex2f(px, py);
          }
          glEnd();
          break;
        case 4: // Flatten: horizontal line
          glBegin(GL_LINES);
          glVertex2f(gcx - s, gcy);
          glVertex2f(gcx + s, gcy);
          glEnd();
          break;
        }
        glLineWidth(1.0f);
      }

      // Settings buttons (Radius -/+, Strength -/+) as a 2x2 grid below the brushes.
      static const char* kSetLabel[4] = { "R-", "R+", "S-", "S+" };
      for (int s = 0; s < 4; ++s) {
        int idx = TerrainPalette::kRadiusDec + s;
        int rx, ry_top, rw, rh;
        TerrainPalette::GetButtonRect(idx, vw, vh, rx, ry_top, rw, rh);
        float bx = (float)rx;
        float by = (float)(vh - (ry_top + rh));
        float bw = (float)rw, bh = (float)rh;

        glEnable(GL_BLEND);
        glColor4f(0.12f, 0.12f, 0.14f, 0.80f);
        glBegin(GL_QUADS);
        glVertex2f(bx, by);
        glVertex2f(bx + bw, by);
        glVertex2f(bx + bw, by + bh);
        glVertex2f(bx, by + bh);
        glEnd();
        glColor3f(0.55f, 0.55f, 0.6f);
        glLineWidth(1.5f);
        glBegin(GL_LINE_LOOP);
        glVertex2f(bx, by);
        glVertex2f(bx + bw, by);
        glVertex2f(bx + bw, by + bh);
        glVertex2f(bx, by + bh);
        glEnd();
        glLineWidth(1.0f);

        // Label centered in the button (draw_text_sm uses top-down y).
        int lblW = glutBitmapLength(GLUT_BITMAP_HELVETICA_10,
                                    (const unsigned char *)kSetLabel[s]);
        int lx = rx + (rw - lblW) / 2;
        int ly = ry_top + (rh + 8) / 2;
        draw_text_sm(lx, ly, kSetLabel[s], 0.9f, 0.9f, 0.95f);
      }

      // Current radius / strength readout text, left of the settings grid.
      {
        int rx, ry_top, rw, rh;
        TerrainPalette::GetButtonRect(TerrainPalette::kRadiusDec, vw, vh, rx, ry_top, rw, rh);
        char rbuf[48], sbuf[48];
        snprintf(rbuf, sizeof(rbuf), "R: %ld", (long)task_tree_view.terrain_brush_radius_);
        snprintf(sbuf, sizeof(sbuf), "S: %ld", (long)task_tree_view.terrain_brush_strength_);
        int rWidth = glutBitmapLength(GLUT_BITMAP_HELVETICA_10,
                                      (const unsigned char *)rbuf);
        int tx = rx - 6 - rWidth;
        if (tx < 2) tx = 2;
        draw_text_sm(tx, ry_top + 10, rbuf, 1.0f, 0.85f, 0.4f);
        draw_text_sm(tx, ry_top + TerrainPalette::kSetBtnH + TerrainPalette::kSetGap + 10,
                     sbuf, 1.0f, 0.85f, 0.4f);
      }
    }

    // Watermark — centered, versioned
    {
      static std::string s_ver_watermark;
      if (s_ver_watermark.empty())
        s_ver_watermark = "IGI Editor v" + Utils::GetVersionString() + " - HeavenHM";
      int wm_w = glutBitmapLength(GLUT_BITMAP_HELVETICA_12,
                                  (const unsigned char *)s_ver_watermark.c_str());
      int wm_x = (params.view_define_->viewport_width_ - wm_w) / 2;
      draw_text(wm_x, params.view_define_->viewport_height_ - 20, s_ver_watermark.c_str(), 0.7f, 0.7f, 0.7f);
    }

    // Animation status panel (bottom-left)
    if (task_tree_view.anim_debug_visible_ && !task_tree_view.anim_status_.empty()) {
      const int aX = 10;
      const int aY = 80;
      const int aW = 360;
      const int aRowH = 16;
      int lines = 1;
      for (char c : task_tree_view.anim_status_) if (c == '\n') lines++;
      const int aH = 28 + lines * aRowH + 6;
      const int vph = params.view_define_->viewport_height_;

      // Background
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glColor4f(0.04f, 0.04f, 0.05f, 0.78f);
      glBegin(GL_QUADS);
      glVertex2f((float)aX, (float)(vph - aY));
      glVertex2f((float)(aX + aW), (float)(vph - aY));
      glVertex2f((float)(aX + aW), (float)(vph - (aY + aH)));
      glVertex2f((float)aX, (float)(vph - (aY + aH)));
      glEnd();

      // Border
      glColor4f(0.3f, 0.8f, 0.3f, 0.9f);
      glLineWidth(1.0f);
      glBegin(GL_LINE_LOOP);
      glVertex2f((float)aX, (float)(vph - aY));
      glVertex2f((float)(aX + aW), (float)(vph - aY));
      glVertex2f((float)(aX + aW), (float)(vph - (aY + aH)));
      glVertex2f((float)aX, (float)(vph - (aY + aH)));
      glEnd();

      // Title
      const char* title = task_tree_view.anim_playing_ ? "Animations Playing" : "Animations Loaded";
      draw_text_sm(aX + 6, aY - 4, title, 0.3f, 0.8f, 0.3f);

      // Status text
      draw_text_sm(aX + 6, aY - 22, task_tree_view.anim_status_.c_str(), 0.7f, 0.9f, 0.7f);
      glDisable(GL_BLEND);
    }

    if (task_tree_view.task_picker_open_ && task_tree_view.level_objects_) {
      int picker_x = 20;
      int picker_w = 520;
      int viewport_h = params.view_define_->viewport_height_;

      // Proportional card layout:
      // Header: top-down 20 to 50 (bottom-up viewport_h - 50 to viewport_h -
      // 20) Items List: top-down 50 to viewport_h - 50 (bottom-up 50 to
      // viewport_h - 50) Footer: top-down viewport_h - 50 to viewport_h - 20
      // (bottom-up 20 to 50)
      int card_top_y = viewport_h - 20; // bottom-up
      int card_bottom_y = 20;           // bottom-up
      int picker_h = viewport_h - 100;  // items list area height

      // Translucent white & yellow background (white and yellow transparent)
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glColor4f(0.0f, 0.0f, 0.0f, 0.45f);
      glBegin(GL_QUADS);
      glVertex2i(picker_x, card_bottom_y);
      glVertex2i(picker_x + picker_w, card_bottom_y);
      glVertex2i(picker_x + picker_w, card_top_y);
      glVertex2i(picker_x, card_top_y);
      glEnd();

      // Warm Yellow/Gold transparent border
      glColor4f(1.0f, 0.85f, 0.0f, 0.7f); // Warm yellow/gold border
      glLineWidth(2.0f);
      glBegin(GL_LINE_LOOP);
      glVertex2i(picker_x, card_bottom_y);
      glVertex2i(picker_x + picker_w, card_bottom_y);
      glVertex2i(picker_x + picker_w, card_top_y);
      glVertex2i(picker_x, card_top_y);
      glEnd();
      glLineWidth(1.0f);

      // Header separator line (at bottom-up viewport_h - 50)
      glColor4f(1.0f, 0.85f, 0.0f, 0.7f);
      glBegin(GL_LINES);
      glVertex2i(picker_x, viewport_h - 50);
      glVertex2i(picker_x + picker_w, viewport_h - 50);
      glEnd();

      // Footer separator line (at bottom-up 50)
      glColor4f(1.0f, 0.85f, 0.0f, 0.7f);
      glBegin(GL_LINES);
      glVertex2i(picker_x, 50);
      glVertex2i(picker_x + picker_w, 50);
      glEnd();

      // Title
      draw_text(picker_x + 15, 38, "Select Task", 1.0f, 0.9f, 0.1f); // Vibrant yellow/gold

      // Search box at top right
      int box_left = picker_x + picker_w - 180;
      int box_right = picker_x + picker_w - 15;

      // Draw Search Box background (semi-transparent warm white)
      glColor4f(1.0f, 1.0f, 1.0f, 0.2f);
      glBegin(GL_QUADS);
      glVertex2i(box_left, viewport_h - 44);
      glVertex2i(box_right, viewport_h - 44);
      glVertex2i(box_right, viewport_h - 26);
      glVertex2i(box_left, viewport_h - 26);
      glEnd();

      // Draw Search Box border (transparent yellow/gold)
      glColor4f(1.0f, 0.85f, 0.0f, 0.7f);
      glBegin(GL_LINE_LOOP);
      glVertex2i(box_left, viewport_h - 44);
      glVertex2i(box_right, viewport_h - 44);
      glVertex2i(box_right, viewport_h - 26);
      glVertex2i(box_left, viewport_h - 26);
      glEnd();

      // Draw Search query or placeholder
      if (task_tree_view.task_picker_search_.empty()) {
        draw_text(box_left + 8, 38, "Search...", 0.5f, 0.5f, 0.5f);
      } else {
        draw_text(box_left + 8, 38, task_tree_view.task_picker_search_.c_str(),
                  1.0f, 1.0f, 1.0f);
      }

      // Footer guidelines
      draw_text(picker_x + 15, viewport_h - 38,
                "[Enter] Insert  [ESC] Cancel  [Type] Search", 0.7f, 0.7f, 0.7f);

      // Build task list mapping exactly like app.cpp
      const auto &objects = task_tree_view.level_objects_->GetObjects();
      std::vector<int> picker_to_objects;

      std::string search_lower = task_tree_view.task_picker_search_;
      std::transform(search_lower.begin(), search_lower.end(),
                     search_lower.begin(),
                     [](unsigned char c) { return std::tolower(c); });

      std::set<std::string> seen_types;
      for (int i = 0; i < (int)objects.size(); ++i) {
        if (!objects[i].deleted) {
          const auto &obj = objects[i];
          if (obj.type == "Task_DeclareParameters") continue; // picker excludes declare params
          std::string type_lower = obj.type;
          std::transform(type_lower.begin(), type_lower.end(), type_lower.begin(),
                         [](unsigned char c) { return std::tolower(c); });
          if (seen_types.count(type_lower) > 0) continue;
          seen_types.insert(type_lower);
          std::string label = obj.type + "()";

          std::string label_lower = label;
          std::transform(label_lower.begin(), label_lower.end(),
                         label_lower.begin(),
                         [](unsigned char c) { return std::tolower(c); });

          if (search_lower.empty() ||
              label_lower.find(search_lower) != std::string::npos) {
            picker_to_objects.push_back(i);
          }
        }
      }

      int row_h = 16;
      int count = (int)picker_to_objects.size();
      int max_visible = std::max(1, picker_h / row_h);

      int start_idx = task_tree_view.task_picker_scroll_offset_;
      int end_idx = std::min(count, start_idx + max_visible);

      if (count == 0) {
        draw_text(picker_x + 22, 70, "No matching tasks found.", 0.6f, 0.6f,
                  0.6f);
      } else {
        for (int i = start_idx; i < end_idx; ++i) {
          int obj_idx = picker_to_objects[i];
          const auto &obj = objects[obj_idx];
          int item_y = 50 + (i - start_idx) * row_h;

          // Background highlight for selected item (transparent yellow/gold)
          if (i == task_tree_view.task_picker_selected_idx_) {
            glEnable(GL_BLEND);
            glColor4f(1.0f, 0.85f, 0.0f,
                      0.35f); // semi-transparent vibrant yellow/gold
            glBegin(GL_QUADS);
            glVertex2i(picker_x + 4, viewport_h - (item_y + row_h));
            glVertex2i(picker_x + picker_w - 4, viewport_h - (item_y + row_h));
            glVertex2i(picker_x + picker_w - 4, viewport_h - item_y);
            glVertex2i(picker_x + 4, viewport_h - item_y);
            glEnd();

            // Arrow highlight pointing to selected line
            draw_text(picker_x + 8, item_y + 11, ">", 1.0f, 0.9f, 0.0f);
          }

          // Format label standard to HUD: Type()
          std::string label = obj.type + "()";

          // Truncate to prevent text overflow
          if (label.size() > 43) {
            label = label.substr(0, 40) + "...";
          }

          // Draw Golden/Yellow Folder Icon
          int folder_x = picker_x + 22;
          int folder_y = item_y;

          glColor3f(1.0f, 0.9f, 0.2f); // Golden/yellow folder color
          glBegin(GL_QUADS);
          glVertex2i(folder_x, viewport_h - (folder_y + 2));
          glVertex2i(folder_x + 12, viewport_h - (folder_y + 2));
          glVertex2i(folder_x + 12, viewport_h - (folder_y + 12));
          glVertex2i(folder_x, viewport_h - (folder_y + 12));
          glEnd();

          // Folder tab
          glBegin(GL_QUADS);
          glVertex2i(folder_x, viewport_h - folder_y);
          glVertex2i(folder_x + 5, viewport_h - folder_y);
          glVertex2i(folder_x + 5, viewport_h - (folder_y + 2));
          glVertex2i(folder_x, viewport_h - (folder_y + 2));
          glEnd();

          // Folder outline
          glColor3f(0.0f, 0.0f, 0.0f);
          glBegin(GL_LINE_LOOP);
          glVertex2i(folder_x, viewport_h - (folder_y + 2));
          glVertex2i(folder_x + 12, viewport_h - (folder_y + 2));
          glVertex2i(folder_x + 12, viewport_h - (folder_y + 12));
          glVertex2i(folder_x, viewport_h - (folder_y + 12));
          glEnd();

          // Select color
          float tr = 1.0f, tg = 1.0f,
                tb = 1.0f; // White text color as requested
          if (i == task_tree_view.task_picker_selected_idx_) {
            tr = 1.0f;
            tg = 0.9f;
            tb = 0.1f; // Yellow highlight for selected text
          }

          draw_text(picker_x + 38, item_y + 11, label.c_str(), tr, tg, tb);
        }
      }

      // Scrollbar on the right edge if tasks count exceeds viewport capacity
      if (count > max_visible) {
        int track_x = picker_x + picker_w - 8;
        int track_h = picker_h - 10;
        int track_y_top = viewport_h - 55;
        int track_y_bottom = track_y_top - track_h;

        // Draw track (translucent white)
        glColor4f(1.0f, 1.0f, 1.0f, 0.15f);
        glBegin(GL_LINES);
        glVertex2i(track_x, track_y_bottom);
        glVertex2i(track_x, track_y_top);
        glEnd();

        // Calculate thumb size and position
        float visible_ratio = (float)max_visible / (float)count;
        int thumb_h = std::max(20, (int)(track_h * visible_ratio));

        float scroll_ratio = 0.0f;
        if (count > max_visible) {
          scroll_ratio = (float)task_tree_view.task_picker_scroll_offset_ /
                         (float)(count - max_visible);
        }
        int thumb_y_top =
            track_y_top - (int)((track_h - thumb_h) * scroll_ratio);
        int thumb_y_bottom = thumb_y_top - thumb_h;

        // Draw thumb (match gold theme border color)
        glColor4f(1.0f, 0.85f, 0.0f, 0.7f);
        glLineWidth(3.0f);
        glBegin(GL_LINES);
        glVertex2i(track_x, thumb_y_bottom);
        glVertex2i(track_x, thumb_y_top);
        glEnd();
        glLineWidth(1.0f);
      }
    }


    // SPR sprite cursors are drawn by App::DrawCustomCursor() — no GLUT overlays here

    if (task_tree_view.pause_mode_) {
      const int viewport_h = params.view_define_->viewport_height_;
      const bool exp = task_tree_view.pause_terrain_expanded_;
      const PauseMenuFontMetrics pause_font_metrics =
          GetPauseMenuFontMetrics(Config::Get().useEditorFont,
                                  Config::Get().systemFontSize);
      const igi::PauseMenuLayout pause_layout = igi::BuildPauseMenuLayout(
          params.view_define_->viewport_width_, viewport_h, exp,
          pause_font_metrics.lineHeight);
      const int menu_w = pause_layout.menu_width;
      const int menu_h = pause_layout.menu_height;
      const int menu_x = pause_layout.menu_left;
      const int menu_y = pause_layout.menu_top;
      const float pause_font_scale = pause_layout.text_scale *
          (static_cast<float>(Config::Get().systemFontSize) / 12.0f);
      const int pause_box_height = (std::max)(24, static_cast<int>(std::ceil(
          static_cast<float>(pause_font_metrics.lineHeight) *
          pause_layout.text_scale)) + 4);
      if (Config::Get().useEditorFont) EnsureEditorFont();
      else EnsureRetailPauseFontTexture();

      auto pause_text_width = [&](const char* text) {
        return PauseMenuTextWidth(text, Config::Get().useEditorFont,
                                  Config::Get().systemFontSize,
                                  pause_layout.text_scale);
      };

      auto draw_pause_text = [&](int x, int y, const char* text,
                                 float r, float g, float b) {
        if (Config::Get().useEditorFont && g_editorFont.valid && g_editorFontTex) {
          DrawFontText(x, viewport_h - y, text, r, g, b, pause_font_scale);
        } else if (!Config::Get().useEditorFont &&
                   g_retailPauseFontTex && GetRetailPauseFont().valid) {
          DrawRetailPauseText(x, viewport_h - y, text, r, g, b,
                              pause_font_scale);
        } else {
          draw_text_sys(x, y, text, r, g, b);
        }
      };

      // Glassmorphism-style background
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glColor4f(0.02f, 0.15f, 0.02f, 0.94f); // Deep emerald
      glBegin(GL_QUADS);
      glVertex2i(menu_x, menu_y);
      glVertex2i(menu_x + menu_w, menu_y);
      glVertex2i(menu_x + menu_w, menu_y + menu_h);
      glVertex2i(menu_x, menu_y + menu_h);
      glEnd();
      glDisable(GL_BLEND);

      // Sharp green border
      glLineWidth(2.5f);
      glColor3f(0.0f, 1.0f, 0.0f);
      glBegin(GL_LINE_LOOP);
      glVertex2i(menu_x, menu_y);
      glVertex2i(menu_x + menu_w, menu_y);
      glVertex2i(menu_x + menu_w, menu_y + menu_h);
      glVertex2i(menu_x, menu_y + menu_h);
      glEnd();

      // Header separator
      glBegin(GL_LINES);
      glVertex2i(menu_x + 10, menu_y + menu_h - 45);
      glVertex2i(menu_x + menu_w - 10, menu_y + menu_h - 45);
      glEnd();
      glLineWidth(1.0f);

      int screen_menu_top = pause_layout.menu_top;
      // Title — centered, bright green
      const char* title = "IGI EDITOR";
      draw_pause_text(menu_x + (menu_w - pause_text_width(title)) / 2,
                 screen_menu_top + 22, title, 0.0f, 1.0f, 0.0f);
      // Subtitle — centered, dim white
      const char* subtitle = "PAUSED";
      draw_pause_text(menu_x + (menu_w - pause_text_width(subtitle)) / 2,
                 screen_menu_top + 46, subtitle, 0.8f, 0.8f, 0.8f);

      // Font row is rendered specially (index 1): a "Font: <type>" toggle on the
      // left plus a [-] [size] [+] size control on the right, all on one line.
      char font_btn_label[32];
      snprintf(font_btn_label, sizeof(font_btn_label), "Font: %s",
               Config::Get().useEditorFont ? "Editor" : "System");
      char mode_btn_label[64];
      snprintf(mode_btn_label, sizeof(mode_btn_label), "Mode: [%s]",
               task_tree_view.in_game_mode_ ? "Game Play" : "Editor");
      char clip_btn_label[64];
      snprintf(clip_btn_label, sizeof(clip_btn_label), "Collision: [%s]",
               task_tree_view.noclip_mode_ ? "Clip" : "Normal");
      char lightmap_btn_label[64];
      snprintf(lightmap_btn_label, sizeof(lightmap_btn_label), "Lightmap: [%s]",
               igi::ObjectLightmapManager::Get().GetRenderModeName());

      int mods = task_tree_view.terrain_mod_options_;
      bool tex = (mods & TERRAIN_TEXTURE_MOD) != 0;
      bool hgt = (mods & TERRAIN_HEIGHT_MOD) != 0;
      bool dsc = (mods & TERRAIN_DISCARD_MOD) != 0;

      char bufTex[32], bufHgt[32], bufDsc[32];
      snprintf(bufTex, sizeof(bufTex), "  [%c] Texture", tex ? 'X' : ' ');
      snprintf(bufHgt, sizeof(bufHgt), "  [%c] Height", hgt ? 'X' : ' ');
      snprintf(bufDsc, sizeof(bufDsc), "  [%c] Discard", dsc ? 'X' : ' ');
      int draws = task_tree_view.terrain_draw_options_;
      bool fog = (draws & Renderer_Terrain::DRAW_TERRAIN_OPT_FOG) != 0;
      char bufFog[32];
      snprintf(bufFog, sizeof(bufFog), "  [%c] Fog", fog ? 'X' : ' ');

      std::vector<const char*> btn_labels;
      btn_labels.push_back("Resume");
      const int MODE_ROW = btn_labels.size();
      btn_labels.push_back(mode_btn_label);
      const int CLIP_ROW = btn_labels.size();
      btn_labels.push_back(clip_btn_label);
      const int FONT_ROW = btn_labels.size();
      btn_labels.push_back(font_btn_label);
      const int LEVEL_ROW = btn_labels.size();
      btn_labels.push_back("Select Level");
      const int AUTOSAVE_ROW = btn_labels.size();
      btn_labels.push_back("Auto Save");
      const int LOGGING_ROW = btn_labels.size();
      btn_labels.push_back("Logging");
      const int LOG_LEVEL_ROW = btn_labels.size();
      btn_labels.push_back("Log Level");
      const int MUSIC_ROW = btn_labels.size();
      btn_labels.push_back("Music");
      const int LIGHTMAPS_ROW = btn_labels.size();
      btn_labels.push_back(lightmap_btn_label);
      const int LIGHTMAPS_CALC_ROW = btn_labels.size();
      btn_labels.push_back("Calculate Lightmaps");
      const int TERRAIN_HEADER_ROW = btn_labels.size();

      btn_labels.push_back(exp ? "Terrain Options: [-]" : "Terrain Options: [+]");

      int TERRAIN_TEX_ROW = -1, TERRAIN_HGT_ROW = -1, TERRAIN_DSC_ROW = -1, TERRAIN_FOG_ROW = -1, TERRAIN_FOGINT_ROW = -1;
      if (exp) {
        TERRAIN_TEX_ROW = btn_labels.size(); btn_labels.push_back(bufTex);
        TERRAIN_HGT_ROW = btn_labels.size(); btn_labels.push_back(bufHgt);
        TERRAIN_DSC_ROW = btn_labels.size(); btn_labels.push_back(bufDsc);
        TERRAIN_FOG_ROW = btn_labels.size(); btn_labels.push_back(bufFog);
        TERRAIN_FOGINT_ROW = btn_labels.size(); btn_labels.push_back("Fog Intensity");
      }

      const int RESET_ROW = btn_labels.size();
      btn_labels.push_back("Reset Level");
      const int SAVE_ROW = btn_labels.size();
      btn_labels.push_back("Save Level");
      const int QUIT_ROW = btn_labels.size();
      btn_labels.push_back("Quit");

      const int NUM_BTNS = btn_labels.size();

      auto row_screen_y = [&](int idx) { return igi::PauseMenuRowCenter(pause_layout, idx); };

      auto spinner_box_bounds = [&](int gl_row_y) {
        const int above = pause_box_height / 2;
        const int below = pause_box_height - above;
        return std::pair<int, int>{gl_row_y - above, gl_row_y + below};
      };

      // Shared spinner-box draw helper (reused for FONT_ROW and LEVEL_ROW)
      auto sbox = [&](int x1, int w, const char *txt, int row_top, int row_bot, int lbl_y) {
        glColor3f(0.0f, 0.7f, 0.0f);
        glBegin(GL_LINE_LOOP);
        glVertex2i(x1, row_bot); glVertex2i(x1 + w, row_bot);
        glVertex2i(x1 + w, row_top); glVertex2i(x1, row_top);
        glEnd();
        int tw = pause_text_width(txt);
        draw_pause_text(x1 + (w - tw) / 2, lbl_y, txt, 0.0f, 0.9f, 0.0f);
      };

      for (int i = 0; i < NUM_BTNS; ++i) {
        int screen_btn_y = row_screen_y(i);
        int gl_btn_y = viewport_h - screen_btn_y;

        bool hovered = (task_tree_view.mouse_x_ >= menu_x &&
                        task_tree_view.mouse_x_ <= menu_x + menu_w &&
                        task_tree_view.mouse_y_ >= screen_btn_y - pause_layout.row_hit_radius &&
                        task_tree_view.mouse_y_ <= screen_btn_y + pause_layout.row_hit_radius);

        if (i == MODE_ROW) {
          int tw = pause_text_width(mode_btn_label);
          int gx = menu_x + (menu_w - tw) / 2;
          draw_pause_text(gx, screen_btn_y, mode_btn_label,
                        task_tree_view.in_game_mode_ ? 0.2f : (hovered ? 1.0f : 0.0f),
                        task_tree_view.in_game_mode_ ? 1.0f : (hovered ? 1.0f : 0.85f),
                        task_tree_view.in_game_mode_ ? 0.3f : 0.0f);
        } else if (i == CLIP_ROW) {
          int tw = pause_text_width(clip_btn_label);
          int gx = menu_x + (menu_w - tw) / 2;
          draw_pause_text(gx, screen_btn_y, clip_btn_label,
                        task_tree_view.noclip_mode_ ? 0.85f : (hovered ? 1.0f : 0.0f),
                        task_tree_view.noclip_mode_ ? 0.45f : (hovered ? 1.0f : 0.85f),
                        task_tree_view.noclip_mode_ ? 0.0f : 0.0f);
        } else if (i == FONT_ROW) {
          // "Font: <type>  [-] <n> [+]" — label left, spinner group right; whole row centered
          const int btn_w = 22, val_w = 44;
          const char* lbl = font_btn_label;
          int label_px = pause_text_width(lbl);
          const igi::PauseMenuSpinner spinner = igi::BuildPauseMenuSpinner(
              pause_layout, screen_btn_y, label_px, val_w);
          draw_pause_text(spinner.group_left, screen_btn_y, lbl,
                        hovered ? 1.0f : 0.0f, hovered ? 1.0f : 0.85f, 0.0f);
          int minus_x = spinner.minus_left;
          int box_x   = spinner.value_left;
          int plus_x  = spinner.plus_left;
          const auto [rt, rb] = spinner_box_bounds(gl_btn_y);
          char szbuf[8]; snprintf(szbuf, sizeof(szbuf), "%d", Config::Get().systemFontSize);
          sbox(minus_x, btn_w, "-",    rt, rb, screen_btn_y);
          sbox(box_x,   val_w, szbuf,  rt, rb, screen_btn_y);
          sbox(plus_x,  btn_w, "+",    rt, rb, screen_btn_y);

        } else if (i == LEVEL_ROW) {
          // Level spinner: "Select Level  [-] [N] [+]" — same width as Font row for visual alignment
          const int btn_w = 22, val_w = 44;
          const char* lbl = "Select Level";
          int label_px = pause_text_width(lbl);
          const igi::PauseMenuSpinner spinner = igi::BuildPauseMenuSpinner(
              pause_layout, screen_btn_y, label_px, val_w);
          draw_pause_text(spinner.group_left, screen_btn_y, lbl,
                        hovered ? 1.0f : 0.0f, hovered ? 1.0f : 0.85f, 0.0f);
          int minus_x = spinner.minus_left;
          int box_x   = spinner.value_left;
          int plus_x  = spinner.plus_left;
          const auto [rt, rb] = spinner_box_bounds(gl_btn_y);
          sbox(minus_x, btn_w, "-",   rt, rb, screen_btn_y);
          sbox(box_x,   val_w, task_tree_view.pause_level_input_.c_str(), rt, rb, screen_btn_y);
          sbox(plus_x,  btn_w, "+",   rt, rb, screen_btn_y);

        } else if (i == AUTOSAVE_ROW) {
          // Auto Save: "Save Enable/Disable  [-] [Ns] [+]" — same width as Font/Level for alignment
          const int btn_w = 22, val_w = 44;
          const char* lbl = task_tree_view.auto_save_enabled_
                                 ? "Save Enable" : "Save Disable";
          int label_px = pause_text_width(lbl);
          const igi::PauseMenuSpinner spinner = igi::BuildPauseMenuSpinner(
              pause_layout, screen_btn_y, label_px, val_w);
          draw_pause_text(spinner.group_left, screen_btn_y, lbl,
                        hovered ? 1.0f : 0.0f, hovered ? 1.0f : 0.85f, 0.0f);
          int minus_x = spinner.minus_left;
          int box_x   = spinner.value_left;
          int plus_x  = spinner.plus_left;
          const auto [rt, rb] = spinner_box_bounds(gl_btn_y);
          char secbuf[16];
          snprintf(secbuf, sizeof(secbuf), "%ds", task_tree_view.auto_save_interval_seconds_);
          sbox(minus_x, btn_w, "-",     rt, rb, screen_btn_y);
          sbox(box_x,   val_w, secbuf,  rt, rb, screen_btn_y);
          sbox(plus_x,  btn_w, "+",     rt, rb, screen_btn_y);

        } else if (i == LOGGING_ROW) {
          if (hovered) {
            glEnable(GL_BLEND);
            glColor4f(0.0f, 0.8f, 0.0f, 0.35f);
            glBegin(GL_QUADS);
            glVertex2i(menu_x, gl_btn_y - 15); glVertex2i(menu_x + menu_w, gl_btn_y - 15);
            glVertex2i(menu_x + menu_w, gl_btn_y + 15); glVertex2i(menu_x, gl_btn_y + 15);
            glEnd();
            glDisable(GL_BLEND);
          }
          char loggingbuf[24];
          snprintf(loggingbuf, sizeof(loggingbuf), "[%c] Logging",
                   task_tree_view.logging_enabled_ ? 'X' : ' ');
          const int tw = pause_text_width(loggingbuf);
          draw_pause_text(menu_x + (menu_w - tw) / 2, screen_btn_y, loggingbuf,
                          hovered ? 1.0f : 0.0f, hovered ? 1.0f : 0.85f, 0.0f);

        } else if (i == LOG_LEVEL_ROW) {
          const int btn_w = 22, val_w = 78;
          const char* lbl = "Log Level";
          const igi::PauseMenuSpinner spinner = igi::BuildPauseMenuSpinner(
              pause_layout, screen_btn_y, pause_text_width(lbl), val_w);
          draw_pause_text(spinner.group_left, screen_btn_y, lbl,
                          hovered ? 1.0f : 0.0f, hovered ? 1.0f : 0.85f, 0.0f);
          const auto [rt, rb] = spinner_box_bounds(gl_btn_y);
          sbox(spinner.minus_left, btn_w, "-", rt, rb, screen_btn_y);
          sbox(spinner.value_left, val_w,
               igi::LogLevelLabel(task_tree_view.log_level_threshold_), rt, rb, screen_btn_y);
          sbox(spinner.plus_left, btn_w, "+", rt, rb, screen_btn_y);

        } else if (i == MUSIC_ROW) {
          // Music on/off checkbox: "[X] Music" / "[ ] Music", centered, hover-highlighted.
          if (hovered) {
            glEnable(GL_BLEND);
            glColor4f(0.0f, 0.8f, 0.0f, 0.35f);
            glBegin(GL_QUADS);
            glVertex2i(menu_x, gl_btn_y - 15); glVertex2i(menu_x + menu_w, gl_btn_y - 15);
            glVertex2i(menu_x + menu_w, gl_btn_y + 15); glVertex2i(menu_x, gl_btn_y + 15);
            glEnd();
            glDisable(GL_BLEND);
          }
          char musicbuf[24];
          snprintf(musicbuf, sizeof(musicbuf), "[%c] Music", task_tree_view.music_on_ ? 'X' : ' ');
          int tw = pause_text_width(musicbuf);
          draw_pause_text(menu_x + (menu_w - tw) / 2, screen_btn_y, musicbuf,
                        hovered ? 1.0f : 0.0f, hovered ? 1.0f : 0.85f, 0.0f);

        } else if (i == LIGHTMAPS_ROW) {
          if (hovered) {
            glEnable(GL_BLEND);
            glColor4f(0.0f, 0.8f, 0.0f, 0.35f);
            glBegin(GL_QUADS);
            glVertex2i(menu_x, gl_btn_y - 15); glVertex2i(menu_x + menu_w, gl_btn_y - 15);
            glVertex2i(menu_x + menu_w, gl_btn_y + 15); glVertex2i(menu_x, gl_btn_y + 15);
            glEnd();
            glDisable(GL_BLEND);
          }
          int lmtw = pause_text_width(lightmap_btn_label);
          draw_pause_text(menu_x + (menu_w - lmtw) / 2, screen_btn_y, lightmap_btn_label,
                        hovered ? 1.0f : 0.0f, hovered ? 1.0f : 0.85f, 0.0f);

        } else if (i == LIGHTMAPS_CALC_ROW) {
          if (hovered) {
            glEnable(GL_BLEND);
            glColor4f(0.0f, 0.8f, 0.0f, 0.35f);
            glBegin(GL_QUADS);
            glVertex2i(menu_x, gl_btn_y - 15); glVertex2i(menu_x + menu_w, gl_btn_y - 15);
            glVertex2i(menu_x + menu_w, gl_btn_y + 15); glVertex2i(menu_x, gl_btn_y + 15);
            glEnd();
            glDisable(GL_BLEND);
          }
          const char* calc_lbl = "Calculate Lightmaps";
          int btw = pause_text_width(calc_lbl);
          draw_pause_text(menu_x + (menu_w - btw) / 2, screen_btn_y, calc_lbl,
                        hovered ? 1.0f : 0.0f, hovered ? 1.0f : 0.85f, 0.0f);

        } else if (i == TERRAIN_HEADER_ROW) {
          draw_pause_text(menu_x + (menu_w - pause_text_width(btn_labels[i])) / 2,
                        screen_btn_y, btn_labels[i], 0.0f, 0.8f, 0.0f);
        } else if (i == TERRAIN_FOGINT_ROW) {
          // Fog Intensity: "Fog Intensity  [-] [N%] [+]" — same spinner layout as Auto Save
          const int btn_w = 22, val_w = 56;
          const char* lbl = "Fog Intensity";
          int label_px = pause_text_width(lbl);
          const igi::PauseMenuSpinner spinner = igi::BuildPauseMenuSpinner(
              pause_layout, screen_btn_y, label_px, val_w);
          draw_pause_text(spinner.group_left, screen_btn_y, lbl,
                        hovered ? 1.0f : 0.0f, hovered ? 1.0f : 0.85f, 0.0f);
          int minus_x = spinner.minus_left;
          int box_x   = spinner.value_left;
          int plus_x  = spinner.plus_left;
          const auto [rt, rb] = spinner_box_bounds(gl_btn_y);
          char fibuf[8];
          snprintf(fibuf, sizeof(fibuf), "%d%%", Config::Get().fogIntensity);
          sbox(minus_x, btn_w, "-",    rt, rb, screen_btn_y);
          sbox(box_x,   val_w, fibuf,  rt, rb, screen_btn_y);
          sbox(plus_x,  btn_w, "+",    rt, rb, screen_btn_y);

        } else if (i == TERRAIN_TEX_ROW || i == TERRAIN_HGT_ROW || i == TERRAIN_DSC_ROW) {
          if (hovered) {
            glEnable(GL_BLEND);
            glColor4f(0.0f, 0.8f, 0.0f, 0.35f);
            glBegin(GL_QUADS);
            glVertex2i(menu_x, gl_btn_y - 15); glVertex2i(menu_x + menu_w, gl_btn_y - 15);
            glVertex2i(menu_x + menu_w, gl_btn_y + 15); glVertex2i(menu_x, gl_btn_y + 15);
            glEnd();
            glDisable(GL_BLEND);
          }
          draw_pause_text(menu_x + (menu_w - pause_text_width(btn_labels[i])) / 2,
                        screen_btn_y, btn_labels[i],
                        hovered ? 1.0f : 0.0f, hovered ? 1.0f : 0.85f, 0.0f);

        } else {
          // Plain centered buttons: Resume, Reset Level, Save Level, Quit
          if (hovered) {
            glEnable(GL_BLEND);
            glColor4f(0.0f, 0.8f, 0.0f, 0.35f);
            glBegin(GL_QUADS);
            glVertex2i(menu_x + 20, gl_btn_y - 16);
            glVertex2i(menu_x + menu_w - 20, gl_btn_y - 16);
            glVertex2i(menu_x + menu_w - 20, gl_btn_y + 12);
            glVertex2i(menu_x + 20, gl_btn_y + 12);
            glEnd();
            glDisable(GL_BLEND);
            draw_pause_text(menu_x + (menu_w - pause_text_width(btn_labels[i])) / 2,
                      screen_btn_y, btn_labels[i], 1.0f, 1.0f, 1.0f);
          } else {
             draw_pause_text(menu_x + (menu_w - pause_text_width(btn_labels[i])) / 2,
                      screen_btn_y, btn_labels[i], 0.0f, 0.85f, 0.0f);
          }
        }
      }
    }

    if (task_tree_view.show_debug_) {
      const auto &entries = Logger::Get().GetEntries();
      // Top-left, fully transparent (no panel, no border). Text uses the editor's
      // yellow/white theme: white for info, yellow for warnings, red for errors.
      const int debug_x = 10;
      const int startY  = 10;     // draw_text_sys uses top-left origin
      const int line_height = 14;
      const int max_lines = 30;
      const int max_chars = 110;

      // Show the last max_lines entries oldest→newest (newest at the bottom).
      int total = (int)entries.size();
      int first = std::max(0, total - max_lines);
      int row = 0;
      for (int i = first; i < total; ++i, ++row) {
        const auto &e = entries[i];
        float r = 1.0f, g = 1.0f, b = 1.0f;   // INFO / default: white
        if (e.level == LogLevel::ERR)          { r = 1.0f; g = 0.3f; b = 0.3f; }
        else if (e.level == LogLevel::FATAL)   { r = 1.0f; g = 0.0f; b = 0.0f; }
        else if (e.level == LogLevel::WARNING) { r = 1.0f; g = 0.85f; b = 0.1f; } // yellow
        else if (e.level == LogLevel::DEBUG)   { r = 0.6f; g = 0.8f; b = 1.0f; }
        std::string msg = e.message;
        if ((int)msg.length() > max_chars) msg = msg.substr(0, max_chars - 3) + "...";
        draw_text_sys(debug_x, startY + row * line_height, msg.c_str(), r, g, b);
      }
    }

    if (task_tree_view.show_help_) {
      const int vw = params.view_define_->viewport_width_;
      const int vh = params.view_define_->viewport_height_;
      const int menu_w  = 560;
      const int menu_h  = vh - 80;
      const int menu_x  = (vw - menu_w) / 2;
      const int menu_y  = 40; // top in screen coords
      const int row_h   = uiFontMetrics.rowHeight;
      const int pad     = 12;
      const int bar_w   = 14;  // vertical scrollbar width
      const int content_x = menu_x + pad;
      const int content_w = menu_w - 2 * pad - bar_w - 4;
      const int content_top = menu_y + 32;    // below title
      const int content_bot = menu_y + menu_h - 20;
      const int max_rows = (content_bot - content_top) / row_h;

      // Background — same semi-transparent dark as the TaskTree panel
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glColor4f(0.04f, 0.06f, 0.04f, 0.82f);  // dark green-tint, slightly transparent
      {
        int gy1 = vh - menu_y, gy2 = vh - (menu_y + menu_h);
        glBegin(GL_QUADS);
        glVertex2i(menu_x, gy2); glVertex2i(menu_x + menu_w, gy2);
        glVertex2i(menu_x + menu_w, gy1); glVertex2i(menu_x, gy1);
        glEnd();
      }
      glDisable(GL_BLEND);
      // Bright green border (matches TaskTree style)
      glColor3f(0.0f, 1.0f, 0.0f);
      glLineWidth(2.0f);
      {
        int gy1 = vh - menu_y, gy2 = vh - (menu_y + menu_h);
        glBegin(GL_LINE_LOOP);
        glVertex2i(menu_x, gy2); glVertex2i(menu_x + menu_w, gy2);
        glVertex2i(menu_x + menu_w, gy1); glVertex2i(menu_x, gy1);
        glEnd();
      }
      glLineWidth(1.0f);
      // Yellow title, like TaskTree selected-item colour
      draw_text(menu_x + menu_w / 2 - 96, menu_y + 18,
                "KEYBINDINGS  [H / Ctrl+H to close]", 1.0f, 1.0f, 0.0f);

      // Draw visible rows from help_entries_
      const std::vector<std::string>* entries = task_tree_view.help_entries_;
      int scroll = task_tree_view.help_scroll_offset_;
      int total  = entries ? (int)entries->size() : 0;
      int clamped_scroll = std::max(0, std::min(scroll, std::max(0, total - max_rows)));

      for (int r = 0; r < max_rows && (clamped_scroll + r) < total; ++r) {
        const std::string& line = (*entries)[clamped_scroll + r];
        int ly = content_top + r * row_h;
        // Alternate yellow / light-yellow rows like the TaskTree selection highlight
        float lr = 1.0f, lg = (r & 1) ? 0.85f : 1.0f, lb = 0.0f;
        draw_text(content_x, ly + row_h - 4, line.c_str(), lr, lg, lb);
      }

      // Vertical scrollbar track + thumb
      if (total > max_rows) {
        int bar_x  = menu_x + menu_w - pad - bar_w;
        int bar_y1 = content_top;
        int bar_y2 = content_bot;
        int bar_gl_y1 = vh - bar_y1, bar_gl_y2 = vh - bar_y2;
        // Track
        glEnable(GL_BLEND);
        glColor4f(0.2f, 0.2f, 0.3f, 0.8f);
        glBegin(GL_QUADS);
        glVertex2i(bar_x, bar_gl_y2); glVertex2i(bar_x + bar_w, bar_gl_y2);
        glVertex2i(bar_x + bar_w, bar_gl_y1); glVertex2i(bar_x, bar_gl_y1);
        glEnd();
        // Thumb
        int track_h = bar_y2 - bar_y1;
        int thumb_h = std::max(20, track_h * max_rows / total);
        int thumb_top = bar_y1 + (track_h - thumb_h) * clamped_scroll / std::max(1, total - max_rows);
        int th_gl_y1 = vh - thumb_top, th_gl_y2 = vh - (thumb_top + thumb_h);
        glColor4f(0.5f, 0.7f, 1.0f, 0.9f);
        glBegin(GL_QUADS);
        glVertex2i(bar_x, th_gl_y2); glVertex2i(bar_x + bar_w, th_gl_y2);
        glVertex2i(bar_x + bar_w, th_gl_y1); glVertex2i(bar_x, th_gl_y1);
        glEnd();
        glDisable(GL_BLEND);
      }
    }

    // ── C2: Property panel (left side, replaces tree) ───────────────────────────
    if (task_tree_view.prop_editor_open_ && task_tree_view.selected_object_index_ >= 0 && task_tree_view.level_objects_) {
      const auto& objects = task_tree_view.level_objects_->GetObjects();
      int sel = task_tree_view.selected_object_index_;
      if (sel < (int)objects.size()) {
        const auto& obj = objects[sel];
        const TaskSchema* scp = GetSchema(obj.type);
        if (scp) {
          const TaskSchema& schema = *scp;
          int vh = params.view_define_->viewport_height_;

          // Gather editable child-task sections (weapon/ammo/AI sub-tasks). Scoped to
          // AI/soldier-family selections only — every other object type (Building,
          // EditRigidObj, etc.) shows ONLY its own fields. Without this gate, ANY
          // selected object with schema-having children (e.g. a Building's nested
          // LightmapInfo) pulled those children's fields into the panel too, even
          // though they're not meant to be edited inline for non-AI objects.
          std::vector<std::pair<int,const TaskSchema*>> child_schemas; // (child obj idx, schema)
          if (task_tree_view.selected_obj_is_ai) {
            for (int ci : obj.childrenIndices) {
                if (ci < 0 || ci >= (int)objects.size()) continue;
                const auto& child = objects[ci];
                if (child.deleted) continue;
                const TaskSchema* cscp = GetSchema(child.type);
                if (cscp && !cscp->empty()) child_schemas.push_back({ci, cscp});
            }
          }

          bool showLightmapButton = (obj.type == "Building" || obj.type == "EditRigidObj") && !obj.isAttaProxy;
          PropPanel::Layout L = PropPanel::BuildLayout(schema, task_tree_view.selected_obj_is_ai, child_schemas,
                                                        task_tree_view.prop_anim_bone_hierarchy_,
                                                        task_tree_view.prop_anim_ids_,
                                                        showLightmapButton,
                                                        uiFontMetrics.rowHeight,
                                                        uiFontMetrics.boxHeight);

          // Apply vertical scroll: shift all widget Y positions.
          const int scroll = task_tree_view.prop_panel_scroll_;
          if (scroll > 0) {
              for (auto& w : L.widgets) { w.y1 -= scroll; w.y2 -= scroll; }
          }

          // GL y for a screen-top-down y.
          auto gl_y = [&](int sy) { return vh - sy; };

          // Enable scissor clipping so scrolled content doesn't bleed outside the panel.
          const int panel_vis_h = vh - PropPanel::kTop;
          glEnable(GL_SCISSOR_TEST);
          glScissor(PropPanel::kLeft, 0, PropPanel::kWidth + 20, panel_vis_h);
          // Filled quad in screen coords.
          auto quad = [&](int x1, int sy1, int x2, int sy2, float r, float g, float b, float a) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glColor4f(r, g, b, a);
            int gy1 = gl_y(sy1), gy2 = gl_y(sy2);
            glBegin(GL_QUADS);
            glVertex2i(x1, gy1); glVertex2i(x2, gy1);
            glVertex2i(x2, gy2); glVertex2i(x1, gy2);
            glEnd();
            glDisable(GL_BLEND);
          };
          auto border = [&](int x1, int sy1, int x2, int sy2, float r, float g, float b) {
            glColor3f(r, g, b);
            int gy1 = gl_y(sy1), gy2 = gl_y(sy2);
            glBegin(GL_LINE_LOOP);
            glVertex2i(x1, gy1); glVertex2i(x2, gy1);
            glVertex2i(x2, gy2); glVertex2i(x1, gy2);
            glEnd();
          };

          // Current render target — reassigned per object (parent or a child) so
          // the shared field renderer reads values from / scopes edits to the right
          // LevelObject. This is what gives children the identical parent interface.
          const LevelObject* curObj = &obj;
          const TaskSchema*  curSchema = &schema;
          int curObjIdx = sel;
          // Does the active text-edit / drag target the object currently drawn?
          // prop_*_obj_index_ < 0 means "the parent" (selected object).
          auto resolveEdit = [&](int tIdx) {
            return task_tree_view.prop_edit_obj_index_ < 0 ? (tIdx == sel)
                 : (task_tree_view.prop_edit_obj_index_ == tIdx);
          };
          auto resolveDrag = [&](int tIdx) {
            return task_tree_view.prop_drag_obj_index_ < 0 ? (tIdx == sel)
                 : (task_tree_view.prop_drag_obj_index_ == tIdx);
          };

          auto tok = [&](int idx) -> std::string {
            return (idx >= 0 && idx < (int)curObj->argTokens.size()) ? curObj->argTokens[idx] : std::string("-");
          };

          // Like tok() but strips surrounding double-quotes for display.
          auto display_tok = [&](int idx) -> std::string {
            std::string v = tok(idx);
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
              return v.substr(1, v.size() - 2);
            return v;
          };

          // Caret blink (on/off ~2Hz). ~7px per char matches the wrap heuristic.
          const bool caret_on = ((glutGet(GLUT_ELAPSED_TIME) / 500) & 1) == 0;
          int caret_idx = task_tree_view.prop_text_caret_;
          if (caret_idx < 0) caret_idx = 0;
          if (caret_idx > (int)task_tree_view.prop_text_buf_.size())
            caret_idx = (int)task_tree_view.prop_text_buf_.size();
          // Draw an editable box's contents + blinking caret. `field_id` is the
          // edit-field key (fi*3+comp, or -2 for the note). `multiline` wraps.
          // Returns flat text offsets of each visual line start.
          // Lines split on '\n'; lines longer than max_chars wrap.
          auto aiLineStarts = [](const std::string& t, int mc) {
            std::vector<int> s; s.push_back(0);
            for (int i = 0; i < (int)t.size(); ) {
              if (t[i] == '\n') { s.push_back(i + 1); i++; }
              else {
                int cnt = 0;
                while (i < (int)t.size() && t[i] != '\n' && cnt < mc) { i++; cnt++; }
                if (i < (int)t.size() && t[i] != '\n') s.push_back(i);
              }
            }
            return s;
          };

          // draw_edit_box: start_line = first visible visual line (scroll offset for multiline).
          //                hscroll    = first visible char offset (scroll offset for single-line).
          auto draw_edit_box = [&](const PropPanel::Widget& w, int field_id,
                                   const std::string& live_text, bool multiline,
                                   int start_line = 0, int hscroll = 0) {
            bool editing = (resolveEdit(curObjIdx) && task_tree_view.prop_text_edit_field_ == field_id);
            const std::string& txt = editing ? task_tree_view.prop_text_buf_ : live_text;
            const int cw = uiFontMetrics.charWidth;
            int max_chars = std::max(1, (w.x2 - w.x1 - 6) / cw);
            int box_lines_cap = std::max(1, (w.y2 - w.y1) / L.box_h);

            // Selection range for THIS field (if active). anchor == focus
            // means caret-only, no highlight.
            int sel_a = -1, sel_b = -1;
            if (editing && task_tree_view.prop_text_sel_anchor_ >= 0
                && task_tree_view.prop_text_sel_focus_  >= 0
                && task_tree_view.prop_text_sel_anchor_ != task_tree_view.prop_text_sel_focus_) {
              sel_a = std::min(task_tree_view.prop_text_sel_anchor_,
                               task_tree_view.prop_text_sel_focus_);
              sel_b = std::max(task_tree_view.prop_text_sel_anchor_,
                               task_tree_view.prop_text_sel_focus_);
              sel_a = std::max(0, std::min(sel_a, (int)txt.size()));
              sel_b = std::max(0, std::min(sel_b, (int)txt.size()));
            }

            if (!multiline) {
              // Single-line with horizontal scroll
              int hs = std::max(0, std::min(hscroll, (int)txt.size()));
              std::string disp = txt.size() > (size_t)hs ? txt.substr(hs, max_chars) : "";
              // Selection highlight (single-line)
              if (sel_a < sel_b) {
                int vis_a = std::max(0, sel_a - hs);
                int vis_b = std::max(0, sel_b - hs);
                vis_a = std::min(vis_a, (int)disp.size());
                vis_b = std::min(vis_b, (int)disp.size());
                if (vis_a < vis_b) {
                  std::string beforeA = disp.substr(0, vis_a);
                  std::string beforeB = disp.substr(0, vis_b);
                  int xa = w.x1 + 3 + measure_text_width(beforeA.c_str(), (int)beforeA.size());
                  int xb = w.x1 + 3 + measure_text_width(beforeB.c_str(), (int)beforeB.size());
                  glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                  glColor4f(0.3f, 0.6f, 1.0f, 0.45f); // pale blue highlight
                  // OpenGL Y is bottom-up — convert top-down `w.y1` via gl_y.
                  int yt_top_gl = gl_y(w.y1 + 3);
                  int yt_bot    = gl_y(w.y1 + 16);
                  glBegin(GL_QUADS);
                  glVertex2i(xa, yt_top_gl); glVertex2i(xb, yt_top_gl);
                  glVertex2i(xb, yt_bot);    glVertex2i(xa, yt_bot);
                  glEnd();
                  glDisable(GL_BLEND);
                }
              }
              draw_text(w.x1 + 3, w.y1 + 12, disp.c_str(), 1.0f, 1.0f, 0.85f);
              if (editing && caret_on) {
                int vis = std::max(0, caret_idx - hs);
                vis = std::min(vis, (int)disp.size());
                std::string before = disp.substr(0, vis);
                int cx = w.x1 + 3 + measure_text_width(before.c_str(), (int)before.size());
                glColor3f(1.0f, 0.95f, 0.2f);
                glBegin(GL_LINES);
                glVertex2i(cx, gl_y(w.y1 + 3)); glVertex2i(cx, gl_y(w.y1 + 16));
                glEnd();
              }
              return;
            }

            // Multiline: build visual line starts respecting \n and max_chars wrap
            auto lstarts = aiLineStarts(txt, max_chars);
            // Caret visual line/col
            int caret_line = -1, caret_col = -1;
            if (editing) {
              caret_line = (int)(std::upper_bound(lstarts.begin(), lstarts.end(), caret_idx) - lstarts.begin()) - 1;
              caret_line = std::max(0, std::min(caret_line, (int)lstarts.size() - 1));
              caret_col  = caret_idx - lstarts[caret_line];
            }
            // Selection highlight pass: one translucent rectangle per visible
            // line that overlaps the [sel_a, sel_b) range. Drawn BEFORE the
            // text so the white characters stay readable on top.
            if (sel_a < sel_b) {
              glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
              glColor4f(0.3f, 0.6f, 1.0f, 0.45f);
              int rl = 0;
              for (int li = start_line; li < (int)lstarts.size() && rl < box_lines_cap; li++, rl++) {
                int ls = lstarts[li];
                int le = (li + 1 < (int)lstarts.size()) ? lstarts[li + 1] : (int)txt.size();
                if (le > ls && txt[le - 1] == '\n') le--;
                int line_len = std::max(0, le - ls);
                // Clip the selection to this visual line.
                int hi_a = std::max(sel_a, ls);
                int hi_b = std::min(sel_b, ls + line_len);
                if (hi_a >= hi_b) continue;
                std::string beforeA = txt.substr(ls, std::min(hi_a - ls, line_len));
                std::string beforeB = txt.substr(ls, std::min(hi_b - ls, line_len));
                int xa = w.x1 + 3 + measure_text_width(beforeA.c_str(), (int)beforeA.size());
                int xb = w.x1 + 3 + measure_text_width(beforeB.c_str(), (int)beforeB.size());
                // OpenGL Y is bottom-up — convert top-down `w.y1` via gl_y.
                int yt_top = w.y1 + 3 + rl * L.box_h;
                int yt_bot = gl_y(yt_top + 13);
                int yt_top_gl = gl_y(yt_top);
                glBegin(GL_QUADS);
                glVertex2i(xa, yt_top_gl); glVertex2i(xb, yt_top_gl);
                glVertex2i(xb, yt_bot);    glVertex2i(xa, yt_bot);
                glEnd();
              }
              glDisable(GL_BLEND);
            }
            // Render visible lines from start_line
            int render_line = 0;
            for (int li = start_line; li < (int)lstarts.size() && render_line < box_lines_cap; li++, render_line++) {
              int ls = lstarts[li];
              int le = (li + 1 < (int)lstarts.size()) ? lstarts[li + 1] : (int)txt.size();
              if (le > ls && txt[le - 1] == '\n') le--;
              int seg_len = std::min(le - ls, max_chars);
              if (seg_len < 0) seg_len = 0;
              std::string seg = txt.substr(ls, seg_len);
              draw_text(w.x1 + 3, w.y1 + 12 + render_line * L.box_h, seg.c_str(), 1.0f, 1.0f, 0.85f);
              if (editing && caret_on && caret_line == li) {
                int cc = std::min(caret_col, (int)seg.size());
                std::string before = seg.substr(0, cc);
                int cx  = w.x1 + 3 + measure_text_width(before.c_str(), (int)before.size());
                int cyt = w.y1 + 3 + render_line * L.box_h;
                glColor3f(1.0f, 0.95f, 0.2f);
                glBegin(GL_LINES);
                glVertex2i(cx, gl_y(cyt)); glVertex2i(cx, gl_y(cyt + 13));
                glEnd();
              }
            }
          };

          // Header text — track running screen y to mirror the layout.
          int ty = L.panel_y + PropPanel::kPad;
          char hdr[160];
          snprintf(hdr, sizeof(hdr), "QTasktype: %s", obj.type.c_str());
          draw_text(L.panel_x + PropPanel::kPad, ty + 11, hdr, 1.0f, 1.0f, 1.0f);
          ty += L.row_h;
          draw_text(L.panel_x + PropPanel::kPad, ty + 11, "QTask Note (QTaskNote):", 1.0f, 0.9f, 0.2f);
          ty += L.row_h;

          // Walk widgets; field headers + value labels are derived from layout y.
          // Note box (first widget).
          {
            const auto& w = L.widgets[0];
            bool editing = (task_tree_view.prop_text_edit_field_ == -2);
            quad(w.x1, w.y1, w.x2, w.y2, 0.0f, 0.0f, 0.0f, 0.40f);
            border(w.x1, w.y1, w.x2, w.y2, 1.0f,
                   editing ? 0.95f : 1.0f, editing ? 0.2f : 1.0f);
            draw_edit_box(w, -2, obj.name, false);
          }

          // Shared per-field renderer: used for the parent task AND each child so
          // both present the identical interface (pads, sliders, boxes). Reads from
          // / scopes edits to `tObj`, consuming widgets sequentially from `wi`.
          auto renderFields = [&](const LevelObject& tObj, const TaskSchema& tSchema,
                                  int tObjIdx, int& wi, int& y) {
            curObj = &tObj; curSchema = &tSchema; curObjIdx = tObjIdx;
            auto editVal = [&](int code){ return resolveEdit(tObjIdx) && task_tree_view.prop_text_edit_field_ == code; };
            auto dragVal = [&](int code){ return resolveDrag(tObjIdx) && task_tree_view.prop_field_index_ == code; };
            for (int fi = 0; fi < (int)tSchema.size(); ++fi) {
            const FieldDef& fd = tSchema[fi];
            const std::string& tn = fd.typeName;
            bool is_pos    = (tn == "ObjectPos");
            bool is_float3 = (tn == "Real32x3" || tn == "Real64x3");
            bool is_ori    = (tn == "Real32x9");
            bool is_rgb    = (tn == "RGB" || tn == "Colour");
            bool is_str    = (tn.find("String") != std::string::npos || tn == "VarString" ||
                              tn == "EnumString32" || tn == "DropDownCombo");
            bool is_bool   = (tn == "bool8" || tn == "PushButton");
            bool is_ro     = (tn == "Graph" || tn == "AnimData" || tn == "TrainPos1D");
            bool is_int    = (tn == "Int16" || tn == "Int32" || tn == "EnumInt32");

            // Field header line "Name (Type)(sub):"
            const char* sub = "";
            if (tn == "Angle")        sub = "(Angle)";
            else if (tn == "Degrees") sub = "(Degrees)";
            else if (tn == "PushButton") sub = "(Button)";
            char fhdr[160];
            if (sub[0]) snprintf(fhdr, sizeof(fhdr), "%s (%s)%s:", fd.name.c_str(), tn.c_str(), sub);
            else        snprintf(fhdr, sizeof(fhdr), "%s (%s):", fd.name.c_str(), tn.c_str());
            draw_text(L.panel_x + PropPanel::kPad, y + 11, fhdr, 1.0f, 0.9f, 0.2f);
            y += L.row_h;

            // Helper: editable numeric box (NumBox) — label + box + caret.
            auto draw_numbox = [&](const PropPanel::Widget& w, const char* label) {
              int field_id = fi * 3 + w.comp;
              bool editing = editVal(field_id);
              if (label && label[0])
                draw_text(L.panel_x + PropPanel::kPad, w.y1 + 12, label, 1.0f, 0.9f, 0.2f);
              quad(w.x1, w.y1, w.x2, w.y2, 0.0f, 0.0f, 0.0f, 0.40f);
              border(w.x1, w.y1, w.x2, w.y2, 1.0f,
                     editing ? 0.95f : 1.0f, editing ? 0.2f : 1.0f);
              draw_edit_box(w, field_id, tok(fd.argOffset + w.comp), false);
            };

            if (is_pos) {
              // X/Y/Z editable numeric boxes.
              const char* lab[3] = {"X", "Y", "Z"};
              for (int c = 0; c < 3; ++c) {
                const auto& w = L.widgets[wi++];
                draw_numbox(w, lab[w.comp]);
              }
              // 2D pad
              const auto& pad = L.widgets[wi++];
              quad(pad.x1, pad.y1, pad.x2, pad.y2, 0.0f, 0.0f, 0.0f, 0.40f);
              border(pad.x1, pad.y1, pad.x2, pad.y2, 1.0f, 1.0f, 1.0f);
              {
                double px = 0, py = 0;
                if (editVal(fi * 3 + 0))
                  try { px = std::stod(task_tree_view.prop_text_buf_); } catch(...) {}
                else
                  try { px = std::stod(tok(fd.argOffset + 0)); } catch(...) {}
                if (editVal(fi * 3 + 1))
                  try { py = std::stod(task_tree_view.prop_text_buf_); } catch(...) {}
                else
                  try { py = std::stod(tok(fd.argOffset + 1)); } catch(...) {}
                int cx = pad.x1 + (pad.x2 - pad.x1) / 2;
                int cy = pad.y1 + (pad.y2 - pad.y1) / 2;
                int hx, hy;
                bool is_pad_dragging = (dragVal(fi * 3 + 0) || dragVal(fi * 3 + 1));
                if (is_pad_dragging) {
                  // During drag: show marker at cursor position relative to pad center
                  int half_w = (pad.x2 - pad.x1) / 2;
                  int half_h = (pad.y2 - pad.y1) / 2;
                  int raw_dx = task_tree_view.mouse_x_ - (pad.x1 + half_w);
                  int raw_dy = task_tree_view.mouse_y_ - (pad.y1 + half_h);
                  hx = cx + std::max(-half_w, std::min(half_w, raw_dx));
                  hy = cy + std::max(-half_h, std::min(half_h, raw_dy));
                } else {
                  hx = cx;  // not dragging: show at center
                  hy = cy;
                }
                quad(hx - 4, hy - 4, hx + 4, hy + 4, 1.0f, is_pad_dragging ? 0.95f : 0.85f, is_pad_dragging ? 0.2f : 0.0f, 1.0f);
              }
              // Z vertical slider
              const auto& zs = L.widgets[wi++];
              quad(zs.x1, zs.y1, zs.x2, zs.y2, 0.0f, 0.0f, 0.0f, 0.40f);
              border(zs.x1, zs.y1, zs.x2, zs.y2, 1.0f, 1.0f, 1.0f);
              {
                // Mirror the 2D pad: track the cursor within the slider during a
                // drag, centred when idle. The previous modulo-50 mapping wrapped
                // 0→1 every 50 world units, making the thumb flicker top↔bottom on
                // the huge absolute Z coordinates.
                bool drag = dragVal(fi * 3 + 2);
                int th;
                if (drag) {
                  int raw_dy = task_tree_view.mouse_y_ - zs.y1;
                  th = zs.y1 + std::max(0, std::min(zs.y2 - zs.y1, raw_dy));
                } else {
                  th = zs.y1 + (zs.y2 - zs.y1) / 2;
                }
                quad(zs.x1, th - 3, zs.x2, th + 3, 1.0f, drag ? 0.95f : 0.85f, drag ? 0.2f : 0.0f, 1.0f);
              }
              y = pad.y2 + 6;
              // Snap buttons
              const auto& bg = L.widgets[wi++];
              const auto& bo = L.widgets[wi++];
              quad(bg.x1, bg.y1, bg.x2, bg.y2, 0.0f, 0.0f, 0.0f, 0.40f);
              border(bg.x1, bg.y1, bg.x2, bg.y2, 1.0f, 1.0f, 1.0f);
              draw_text(bg.x1 + 6, bg.y1 + 12, "Snap to ground", 1.0f, 0.9f, 0.2f);
              quad(bo.x1, bo.y1, bo.x2, bo.y2, 0.0f, 0.0f, 0.0f, 0.40f);
              border(bo.x1, bo.y1, bo.x2, bo.y2, 1.0f, 1.0f, 1.0f);
              draw_text(bo.x1 + 6, bo.y1 + 12, "Snap to object", 1.0f, 0.9f, 0.2f);
              y = bg.y2 + 4;
              {
                char ab[80];
                snprintf(ab, sizeof(ab), "Altitude: %.6f meter", tObj.pos.z);
                draw_text(L.panel_x + PropPanel::kPad, y + 11, ab, 0.75f, 0.7f, 0.4f);
                y += L.row_h;
              }
            } else if (is_float3) {
              // Generic 3-component float (Speed, etc.) — three labeled NumBoxes, no pad
              const char* lab[3] = {"X", "Y", "Z"};
              for (int c = 0; c < 3; ++c) {
                const auto& w = L.widgets[wi++];
                draw_numbox(w, lab[c]);
              }
              y = L.widgets[wi - 1].y2;
            } else if (is_ori) {
              // Count how many OriSlider widgets belong to this field (1 for AI-only Gamma, 3 otherwise).
              int ori_count = 0;
              {
                int tmp = wi;
                while (tmp < (int)L.widgets.size() &&
                       L.widgets[tmp].fieldIndex == fi &&
                       L.widgets[tmp].kind == PropPanel::WidgetKind::OriSlider)
                  ++tmp;
                ori_count = tmp - wi;
              }
              const char* lab[3] = {"A", "B", "G"};
              for (int ci = 0; ci < ori_count; ++ci) {
                const auto& w = L.widgets[wi++];
                // Short label on the far left
                draw_text(L.panel_x + PropPanel::kPad, w.y1 + 12, lab[w.comp], 1.0f, 0.9f, 0.2f);
                // Slider track ends 72px before right edge to leave room for value text
                int cy = (w.y1 + w.y2) / 2;
                int track_x2 = w.x2 - 72;
                quad(w.x1, cy - 2, track_x2, cy + 2, 0.0f, 0.0f, 0.0f, 0.40f);
                border(w.x1, cy - 2, track_x2, cy + 2, 1.0f, 1.0f, 1.0f);
                // Slider thumb
                float v = 0.f;
                if (editVal(fi * 3 + w.comp))
                  try { v = std::stof(task_tree_view.prop_text_buf_); } catch(...) {}
                else
                  try { v = std::stof(tok(fd.argOffset + w.comp)); } catch(...) {}
                float norm = std::max(0.f, std::min(1.f, (v + 3.14159f) / (2.f * 3.14159f)));
                int tx = w.x1 + (int)(norm * (track_x2 - w.x1 - 6));
                bool drag = dragVal(fi * 3 + w.comp);
                quad(tx, cy - 5, tx + 6, cy + 5, 1.0f, drag ? 0.95f : 0.85f, drag ? 0.2f : 0.0f, 1.0f);
                // Value text to the right of the slider
                draw_text(track_x2 + 4, w.y1 + 12, tok(fd.argOffset + w.comp).c_str(), 1.0f, 1.0f, 0.85f);
                y = w.y2;
              }
              if (ori_count == 0) y += L.box_h;  // fallback if no widgets
            } else if (is_rgb) {
              const char* rgbl[3] = {"R", "G", "B"};
              float rgb[3] = {0,0,0};
              for (int c = 0; c < 3; ++c) {
                const auto& w = L.widgets[wi++];
                draw_text(L.panel_x + PropPanel::kPad, w.y1 + 12, rgbl[w.comp], 1.0f, 0.9f, 0.2f);
                draw_text(L.panel_x + PropPanel::kPad + 24, w.y1 + 12,
                          tok(fd.argOffset + w.comp).c_str(), 1.0f, 1.0f, 0.85f);
                int cy = (w.y1 + w.y2) / 2;
                quad(w.x1, cy - 2, w.x2, cy + 2, 0.0f, 0.0f, 0.0f, 0.40f);
                border(w.x1, cy - 2, w.x2, cy + 2, 1.0f, 1.0f, 1.0f);
                float v = 0.f; try { v = std::stof(tok(fd.argOffset + w.comp)); } catch(...) {}
                rgb[w.comp] = std::max(0.f, std::min(1.f, v));
                float norm = rgb[w.comp];
                int tx = w.x1 + (int)(norm * (w.x2 - w.x1 - 6));
                bool drag = dragVal(fi * 3 + w.comp);
                quad(tx, cy - 5, tx + 6, cy + 5, 1.0f, drag ? 0.95f : 0.85f, drag ? 0.2f : 0.0f, 1.0f);
                // Swatch to the right of the last slider row.
                if (c == 2) {
                  int sw_x = w.x2 + 4, sw_y1 = L.widgets[wi - 3].y1, sw_y2 = w.y2;
                  quad(sw_x, sw_y1, sw_x + 18, sw_y2, rgb[0], rgb[1], rgb[2], 1.0f);
                  border(sw_x, sw_y1, sw_x + 18, sw_y2, 1.0f, 1.0f, 1.0f);
                }
                y = w.y2;
              }
            } else if (is_str) {
              const auto& w = L.widgets[wi++];
              bool multiline = (tn == "VarString" || tn == "String256");
              int field_id = fi * 3 + 0;
              bool editing = editVal(field_id);
              quad(w.x1, w.y1, w.x2, w.y2, 0.0f, 0.0f, 0.0f, 0.40f);
              border(w.x1, w.y1, w.x2, w.y2, 1.0f,
                     editing ? 0.95f : 1.0f, editing ? 0.2f : 1.0f);
              draw_edit_box(w, field_id, display_tok(fd.argOffset), multiline);
              y = w.y2 + 2;
            } else if (is_bool) {
              const auto& w = L.widgets[wi++];
              bool bv = false; try { bv = (std::stoi(tok(fd.argOffset)) != 0); } catch(...) {}
              // Draw a fixed 14×14 checkbox square; hit area is the full row (w.x1..w.x2)
              int cx2 = w.x1 + 14, cy2 = w.y1 + 14;
              quad(w.x1, w.y1, cx2, cy2, 0.0f, 0.0f, 0.0f, 0.40f);
              if (bv) quad(w.x1 + 2, w.y1 + 2, cx2 - 2, cy2 - 2, 1.0f, 1.0f, 1.0f, 0.9f);
              border(w.x1, w.y1, cx2, cy2, 1.0f, 1.0f, 1.0f);
              draw_text(cx2 + 6, w.y1 + 11, bv ? "TRUE" : "FALSE", 1.0f, 0.9f, 0.2f);
              y = w.y2;
            } else if (is_ro) {
              std::string v = tok(fd.argOffset);
              if (v.size() > 38) v = v.substr(0, 35) + "...";
              draw_text(L.panel_x + PropPanel::kPad + 4, y + 11, v.c_str(), 0.75f, 0.7f, 0.4f);
              y += L.row_h;
            } else if (is_int) {
              const auto& w = L.widgets[wi++];
              draw_numbox(w, nullptr);
              y = w.y2;
            } else {
              // Real32/Real64/RangeReal32/Angle/Degrees: slider + editable box.
              const auto& sl = L.widgets[wi++];   // NumSlider
              const auto& bx = L.widgets[wi++];   // NumBox
              int cy = (sl.y1 + sl.y2) / 2;
              quad(sl.x1, cy - 2, sl.x2, cy + 2, 0.0f, 0.0f, 0.0f, 0.40f);
              border(sl.x1, cy - 2, sl.x2, cy + 2, 1.0f, 1.0f, 1.0f);
              float v = 0.f;
              if (editVal(fi * 3 + 0))
                try { v = std::stof(task_tree_view.prop_text_buf_); } catch(...) {}
              else
                try { v = std::stof(tok(fd.argOffset)); } catch(...) {}
              float norm = std::max(0.f, std::min(1.f, (v - std::floor(v / 200.f) * 200.f) / 200.f));
              int tx = sl.x1 + (int)(norm * (sl.x2 - sl.x1 - 6));
              bool drag = dragVal(fi * 3 + 0);
              quad(tx, cy - 5, tx + 6, cy + 5, 1.0f, drag ? 0.95f : 0.85f, drag ? 0.2f : 0.0f, 1.0f);
              draw_numbox(bx, nullptr);
              y = bx.y2;
            }
            y += 4;  // gap between fields
            }
          }; // end renderFields

          // Render the parent task fields, then each child task with the SAME
          // interface. Child fields are routed to their own LevelObject via the
          // widget objIndex / resolveEdit / resolveDrag, so weapons/AI/EditRigidObj
          // children get pads, sliders and boxes exactly like a top-level task.
          int wi = 1;                        // widget 0 is the parent note box
          // Lightmap button at widget 1 (Building/EditRigidObj) — draw it before
          // the scrollable field sections so it stays near the top.
          if (wi < (int)L.widgets.size() &&
              L.widgets[wi].kind == PropPanel::WidgetKind::LightmapButton) {
              const auto& bw = L.widgets[wi++];
              quad(bw.x1, bw.y1, bw.x2, bw.y2, 0.0f, 0.0f, 0.0f, 0.40f);
              border(bw.x1, bw.y1, bw.x2, bw.y2, 1.0f, 1.0f, 1.0f);
              draw_text(bw.x1 + 6, bw.y1 + 12, "Calculate Light Mapping", 1.0f, 0.9f, 0.2f);
          }
          int y  = L.widgets[wi - 1].y2 + 6;
          renderFields(obj, schema, sel, wi, y);
          for (const auto& [ci, cscp] : child_schemas) {
              if (wi < (int)L.widgets.size() &&
                  L.widgets[wi].kind == PropPanel::WidgetKind::ChildHeader) {
                  const auto& hw = L.widgets[wi++];
                  char sep[96];
                  snprintf(sep, sizeof(sep), "%s", objects[ci].type.c_str());
                  draw_text(hw.x1, hw.y1 + 11, sep, 0.45f, 0.85f, 1.0f);
                  y = hw.y2 + 2;
              }
              renderFields(objects[ci], *cscp, ci, wi, y);
              y += 4;
          }

          // AI script widgets (appended after child fields for AI task types)
          curObjIdx = sel;  // resolveEdit must see sel so edit state is applied correctly
          while (wi < (int)L.widgets.size()) {
              using K = PropPanel::WidgetKind;
              const auto& w = L.widgets[wi++];
              if (w.kind == K::AnimIdButton) {
                  bool isFirst = (wi - 2 < 0) || (L.widgets[wi - 2].kind != K::AnimIdButton);
                  if (isFirst) {
                      char lbl[80];
                      snprintf(lbl, sizeof(lbl), "Bone Hierarchy: %03d.IFF (check an id to play/pause)",
                               task_tree_view.prop_anim_bone_hierarchy_);
                      draw_text(w.x1, w.y1 - L.row_h + 12, lbl, 0.8f, 0.8f, 1.0f);
                  }
                  // Checkbox glyph, left-aligned within the row.
                  const int boxSz = w.y2 - w.y1 - 4;
                  const int boxX1 = w.x1, boxY1 = w.y1 + 2;
                  if (w.comp < 0) {
                      // "No animations found" placeholder — informational only.
                      border(boxX1, boxY1, boxX1 + boxSz, boxY1 + boxSz, 0.5f, 0.5f, 0.5f);
                      draw_text(boxX1 + boxSz + 8, w.y1 + 12, "No animations found", 0.6f, 0.6f, 0.6f);
                  } else {
                      bool isActive  = (w.comp == task_tree_view.prop_anim_active_id_);
                      bool isPlaying = isActive && task_tree_view.prop_anim_is_playing_;
                      border(boxX1, boxY1, boxX1 + boxSz, boxY1 + boxSz, 1.0f, 1.0f, 1.0f);
                      if (isPlaying) {
                          quad(boxX1 + 3, boxY1 + 3, boxX1 + boxSz - 3, boxY1 + boxSz - 3,
                               0.2f, 0.9f, 0.3f, 0.95f);
                      }
                      char idLbl[32];
                      snprintf(idLbl, sizeof(idLbl), "Animation %d%s", w.comp, isPlaying ? "  (playing)" : "");
                      draw_text(boxX1 + boxSz + 8, w.y1 + 12, idLbl, 1.0f, 1.0f, 1.0f);
                  }
              } else if (w.kind == K::AIScriptPath) {
                  bool ed = resolveEdit(sel) &&
                            task_tree_view.prop_text_edit_field_ == PropPanel::kAIScriptPathField;
                   draw_text(w.x1, w.y1 - L.row_h + 12, "AI Script Path:", 0.8f, 0.8f, 1.0f);
                  quad(w.x1, w.y1, w.x2, w.y2, 0.0f, 0.0f, 0.0f, 0.40f);
                  border(w.x1, w.y1, w.x2, w.y2, 1.0f, ed ? 0.95f : 1.0f, ed ? 0.2f : 1.0f);
                  draw_edit_box(w, PropPanel::kAIScriptPathField,
                                task_tree_view.ai_script_path_, false,
                                0, task_tree_view.ai_script_path_hscroll_);
              } else if (w.kind == K::AIScriptText) {
                  bool ed = resolveEdit(sel) &&
                            task_tree_view.prop_text_edit_field_ == PropPanel::kAIScriptTextField;
                  const char* label = task_tree_view.ai_script_dirty_
                                          ? "AI Script (modified -- save to compile):"
                                          : "AI Script:";
                   draw_text(w.x1, w.y1 - L.row_h + 12, label,
                            task_tree_view.ai_script_dirty_ ? 1.0f : 0.8f,
                            task_tree_view.ai_script_dirty_ ? 0.6f : 0.8f,
                            task_tree_view.ai_script_dirty_ ? 0.2f : 1.0f);
                  quad(w.x1, w.y1, w.x2, w.y2, 0.0f, 0.0f, 0.0f, 0.40f);
                  border(w.x1, w.y1, w.x2, w.y2, 1.0f, ed ? 0.95f : 1.0f, ed ? 0.2f : 1.0f);
                  draw_edit_box(w, PropPanel::kAIScriptTextField,
                                task_tree_view.ai_script_text_, true,
                                task_tree_view.ai_script_vscroll_, 0);
              }
              y = w.y2 + 6;
          }

          // ── Scrollbar ────────────────────────────────────────────────────────────
          {
              const int total_h = L.panel_h;   // full unshifted content height
              const int vis_h   = panel_vis_h;
              if (total_h > vis_h) {
                  const int bar_x = PropPanel::kLeft + PropPanel::kWidth + 2;
                  const int bar_w = 6;
                  // Track
                  glColor4f(0.1f, 0.1f, 0.1f, 0.8f);
                  glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                  glBegin(GL_QUADS);
                  glVertex2i(bar_x, gl_y(PropPanel::kTop)); glVertex2i(bar_x+bar_w, gl_y(PropPanel::kTop));
                  glVertex2i(bar_x+bar_w, gl_y(PropPanel::kTop + vis_h)); glVertex2i(bar_x, gl_y(PropPanel::kTop + vis_h));
                  glEnd();
                  // Thumb
                  float thumb_f = (float)vis_h / (float)total_h;
                  int thumb_h  = std::max(20, (int)(vis_h * thumb_f));
                  int thumb_y  = PropPanel::kTop + (int)((float)scroll / (float)(total_h - vis_h) * (vis_h - thumb_h));
                  glColor4f(0.3f, 0.7f, 1.0f, 0.9f);
                  glBegin(GL_QUADS);
                  glVertex2i(bar_x, gl_y(thumb_y)); glVertex2i(bar_x+bar_w, gl_y(thumb_y));
                  glVertex2i(bar_x+bar_w, gl_y(thumb_y + thumb_h)); glVertex2i(bar_x, gl_y(thumb_y + thumb_h));
                  glEnd();
                  glDisable(GL_BLEND);
              }
          }

          glDisable(GL_SCISSOR_TEST);
        }
      }
    }

    // ── C3: Ctrl+F find bar (centered on screen) ───────────────────────────────
    if (task_tree_view.find_open_) {
      int vw = params.view_define_->viewport_width_;
      int vh = params.view_define_->viewport_height_;

      // Centered 400x80 box
      const int bar_w      = 400;
      const int bar_h      = 80;
      const int bar_x      = vw / 2 - bar_w / 2;
      const int bar_gl_y   = vh / 2 - bar_h / 2;   // GL bottom-up origin

      // Dark semi-transparent background
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glColor4f(0.05f, 0.05f, 0.1f, 0.92f);
      glBegin(GL_QUADS);
      glVertex2i(bar_x,         bar_gl_y);
      glVertex2i(bar_x + bar_w, bar_gl_y);
      glVertex2i(bar_x + bar_w, bar_gl_y + bar_h);
      glVertex2i(bar_x,         bar_gl_y + bar_h);
      glEnd();

      // White border
      glColor4f(1.0f, 1.0f, 1.0f, 0.85f);
      glBegin(GL_LINE_LOOP);
      glVertex2i(bar_x,         bar_gl_y);
      glVertex2i(bar_x + bar_w, bar_gl_y);
      glVertex2i(bar_x + bar_w, bar_gl_y + bar_h);
      glVertex2i(bar_x,         bar_gl_y + bar_h);
      glEnd();
      glDisable(GL_BLEND);

      // Convert GL y to screen top-down y for draw_text
      // bar top (screen-top-down) = vh - (bar_gl_y + bar_h)
      int bar_screen_top = vh - (bar_gl_y + bar_h);

      // Title — shows current find mode
      {
        static const char* kTitles[] = {
            "Find task by type / name / ID:",
            "Find text in task parameters:",
            "Find task by ID:",
            "Find task by note / name:",
            "Set Task ID (empty = auto-assign):"
        };
        int mi = task_tree_view.find_mode_;
        if (mi < 0 || mi > 4) mi = 0;
        const char* title = kTitles[mi];
        int tw = glutBitmapLength(GLUT_BITMAP_HELVETICA_12, (const unsigned char*)title);
        draw_text(bar_x + (bar_w - tw) / 2, bar_screen_top + 14, title, 1.0f, 1.0f, 1.0f);
      }

      // Input box background
      {
        int ibx1 = bar_x + 8;
        int ibx2 = bar_x + bar_w - 8;
        int iby1 = bar_gl_y + 10;
        int iby2 = bar_gl_y + 36;
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(0.1f, 0.1f, 0.15f, 0.95f);
        glBegin(GL_QUADS);
        glVertex2i(ibx1, iby1); glVertex2i(ibx2, iby1);
        glVertex2i(ibx2, iby2); glVertex2i(ibx1, iby2);
        glEnd();
        glColor4f(0.7f, 0.7f, 0.7f, 0.9f);
        glBegin(GL_LINE_LOOP);
        glVertex2i(ibx1, iby1); glVertex2i(ibx2, iby1);
        glVertex2i(ibx2, iby2); glVertex2i(ibx1, iby2);
        glEnd();
        glDisable(GL_BLEND);
      }

      // Search text with cursor
      char find_label[256];
      snprintf(find_label, sizeof(find_label), "%s_", task_tree_view.find_query_.c_str());
      // screen y for text inside input box: bar_screen_top + bar_h - 10 - 26 + 16 = bar_screen_top+20
      int input_text_y = bar_screen_top + bar_h - 10 - (bar_h - 36) - 10;
      draw_text(bar_x + 12, input_text_y, find_label, 1.0f, 1.0f, 1.0f);

      // Match / no-match feedback below the input box.
      if (task_tree_view.find_mode_ == 4) {
        // SetId: input-only, no search — show a hint instead of match feedback.
        draw_text(bar_x + 12, bar_screen_top + 68, "[Enter] apply  (empty = auto-assign)", 0.7f, 0.7f, 0.7f);
      } else if (task_tree_view.find_result_idx_ >= 0 && task_tree_view.level_objects_) {
        const auto& objects = task_tree_view.level_objects_->GetObjects();
        int ri = task_tree_view.find_result_idx_;
        if (ri < (int)objects.size()) {
          char match_buf[160];
          snprintf(match_buf, sizeof(match_buf), "%s \"%s\" (ID:%s)  [Enter]",
                   objects[ri].type.c_str(),
                   objects[ri].name.c_str(),
                   objects[ri].taskId.empty() ? "-1" : objects[ri].taskId.c_str());
          draw_text(bar_x + 12, bar_screen_top + 68, match_buf, 0.3f, 1.0f, 0.3f);
        }
      } else if (!task_tree_view.find_query_.empty()) {
        draw_text(bar_x + 12, bar_screen_top + 68, "No match found", 1.0f, 0.4f, 0.4f);
      }
    }

    // ── File dialog (SaveSubTask / LoadSubTask) ─────────────────────────────
    if (task_tree_view.file_dialog_mode_ != 0) {
      int vw = params.view_define_->viewport_width_;
      int vh = params.view_define_->viewport_height_;
      const int dw = 560, dh = 110;
      const int dx = vw / 2 - dw / 2;
      const int dgl = vh / 2 - dh / 2;
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glColor4f(0.05f, 0.05f, 0.1f, 0.92f);
      glBegin(GL_QUADS);
      glVertex2i(dx,dgl); glVertex2i(dx+dw,dgl); glVertex2i(dx+dw,dgl+dh); glVertex2i(dx,dgl+dh);
      glEnd();
      glColor4f(1.0f, 0.85f, 0.0f, 0.7f);
      glBegin(GL_LINE_LOOP);
      glVertex2i(dx,dgl); glVertex2i(dx+dw,dgl); glVertex2i(dx+dw,dgl+dh); glVertex2i(dx,dgl+dh);
      glEnd();
      glDisable(GL_BLEND);
      int dsy = vh - (dgl + dh); // screen top
      static const char* kDlgTitles[] = {"", "Save Task File:", "Save Parent Task File:", "Load Task File:", "Save Objects File:"};
      int mi = task_tree_view.file_dialog_mode_;
      if (mi < 0 || mi > 4) mi = 0;
      draw_text(dx + 10, dsy + 14, kDlgTitles[mi], 1.0f, 0.9f, 0.1f);
      // Input box
      const int ibx1 = dx + 8, ibx2 = dx + dw - 8;
      const int iby1 = dgl + 14, iby2 = dgl + 38;
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glColor4f(0.1f, 0.1f, 0.15f, 0.95f);
      glBegin(GL_QUADS); glVertex2i(ibx1,iby1); glVertex2i(ibx2,iby1); glVertex2i(ibx2,iby2); glVertex2i(ibx1,iby2); glEnd();
      glColor4f(1.0f, 0.85f, 0.0f, 0.7f);
      glBegin(GL_LINE_LOOP); glVertex2i(ibx1,iby1); glVertex2i(ibx2,iby1); glVertex2i(ibx2,iby2); glVertex2i(ibx1,iby2); glEnd();
      glDisable(GL_BLEND);
      int ity = vh - (iby2 + 14);
      char pathbuf[512]; snprintf(pathbuf, sizeof(pathbuf), "%s_", task_tree_view.file_dialog_path_.c_str());
      draw_text(ibx1 + 4, ity, pathbuf, 1.0f, 1.0f, 1.0f);
      draw_text(dx + 10, dsy + dh - 18, "[Enter] Confirm   [Esc] Cancel", 0.6f, 0.6f, 0.6f);
    }

    // ── Autocomplete task picker (right panel, Ctrl+N) ───────────────────────
    if (task_tree_view.ac_task_picker_open_ && task_tree_view.ac_task_items_) {
      int vw = params.view_define_->viewport_width_;
      int vh = params.view_define_->viewport_height_;
      const int pw = 280, px = vw - pw;
      // Build filtered list
      std::vector<std::string> filtered;
      std::string fl = task_tree_view.ac_task_filter_;
      std::transform(fl.begin(), fl.end(), fl.begin(), [](unsigned char c){ return std::tolower(c); });
      for (const auto& item : *task_tree_view.ac_task_items_) {
        if (fl.empty()) { filtered.push_back(item); }
        else {
          std::string il = item;
          std::transform(il.begin(), il.end(), il.begin(), [](unsigned char c){ return std::tolower(c); });
          if (il.find(fl) != std::string::npos) filtered.push_back(item);
        }
      }
      int count = (int)filtered.size();
      const int row_h = uiFontMetrics.rowHeight, hdr_h = 50, ftr_h = 20;
      int body_h = vh - hdr_h - ftr_h;
      int max_vis = std::max(1, body_h / row_h);

      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glColor4f(0.0f, 0.0f, 0.0f, 0.45f);
      glBegin(GL_QUADS); glVertex2i(px,0); glVertex2i(px+pw,0); glVertex2i(px+pw,vh); glVertex2i(px,vh); glEnd();
      glColor4f(1.0f, 0.85f, 0.0f, 0.7f);
      glBegin(GL_LINE_LOOP); glVertex2i(px,0); glVertex2i(px+pw,0); glVertex2i(px+pw,vh); glVertex2i(px,vh); glEnd();
      glDisable(GL_BLEND);

      draw_text(px + 8, 14, "Task Types", 1.0f, 0.9f, 0.1f);

      // Filter box
      const int fbx1 = px + 4, fbx2 = px + pw - 4, fby1 = vh - hdr_h + 4, fby2 = vh - hdr_h + 22;
      glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glColor4f(0.1f, 0.1f, 0.15f, 0.9f);
      glBegin(GL_QUADS); glVertex2i(fbx1,fby1); glVertex2i(fbx2,fby1); glVertex2i(fbx2,fby2); glVertex2i(fbx1,fby2); glEnd();
      glColor4f(1.0f, 0.85f, 0.0f, 0.6f);
      glBegin(GL_LINE_LOOP); glVertex2i(fbx1,fby1); glVertex2i(fbx2,fby1); glVertex2i(fbx2,fby2); glVertex2i(fbx1,fby2); glEnd();
      glDisable(GL_BLEND);
      char fb[64]; snprintf(fb, sizeof(fb), "%s_", task_tree_view.ac_task_filter_.c_str());
      int fty = vh - (fby2) + 4;
      draw_text(fbx1 + 4, fty, fb, 1.0f, 1.0f, 1.0f);

      // Items
      int sel = task_tree_view.ac_task_selected_idx_;
      int scroll = task_tree_view.ac_task_scroll_offset_;
      for (int r = 0; r < max_vis; ++r) {
        int idx = scroll + r;
        if (idx >= count) break;
        int item_sy = hdr_h + r * row_h;
        if (idx == sel) {
          glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
          glColor4f(1.0f, 0.85f, 0.0f, 0.35f);
          glBegin(GL_QUADS);
          int gy1 = vh - item_sy - row_h, gy2 = vh - item_sy;
          glVertex2i(px,gy1); glVertex2i(px+pw,gy1); glVertex2i(px+pw,gy2); glVertex2i(px,gy2);
          glEnd(); glDisable(GL_BLEND);
          draw_text(px + 4, item_sy + 13, filtered[idx].c_str(), 1.0f, 0.9f, 0.1f);
        } else {
          draw_text(px + 4, item_sy + 13, filtered[idx].c_str(), 1.0f, 1.0f, 1.0f);
        }
      }
      draw_text(px + 4, vh - ftr_h + 4, "[Enter] Insert  [Esc] Cancel  [Type] Filter", 0.5f, 0.5f, 0.5f);
    }

    // ── Model ID picker (right panel, Ctrl+O) ───────────────────────────────
    if (task_tree_view.model_picker_open_ && task_tree_view.model_entries_) {
      int vw = params.view_define_->viewport_width_;
      int vh = params.view_define_->viewport_height_;
      const int pw = 280, px = vw - pw;
      // Build filtered sorted list
      std::vector<ModelPickerEntry> filtered;
      std::string fl = task_tree_view.model_picker_filter_;
      std::transform(fl.begin(), fl.end(), fl.begin(), [](unsigned char c){ return std::tolower(c); });
      for (const auto& entry : *task_tree_view.model_entries_) {
        if (fl.empty()) { filtered.push_back(entry); }
        else {
          std::string idl = entry.label;
          std::transform(idl.begin(), idl.end(), idl.begin(), [](unsigned char c){ return std::tolower(c); });
          if (idl.find(fl) != std::string::npos) filtered.push_back(entry);
        }
      }
      int count = (int)filtered.size();
      const int row_h = uiFontMetrics.rowHeight, hdr_h = 50, ftr_h = 20;
      int body_h = vh - hdr_h - ftr_h;
      int max_vis = std::max(1, body_h / row_h);

      // 3D rotating preview of the highlighted model, centered in the editor area
      // (to the left of the picker panel). Drawn first, then 2D HUD state restored.
      {
        int sel0 = task_tree_view.model_picker_selected_;
        if (count > 0 && sel0 >= 0 && sel0 < count) {
          int avail = vw - pw;                       // editor area width (panel excluded)
          int s = (int)(std::min(avail, vh) * 0.6f); // square preview side
          if (s > 64) {
            int vpX = (avail - s) / 2;
            int vpY = (vh - s) / 2;
            static auto t0 = std::chrono::steady_clock::now();
            float t = std::chrono::duration<float>(
                          std::chrono::steady_clock::now() - t0).count();
            objects_.DrawModelPreview(filtered[sel0].modelId, ubo_mats_, vpX, vpY, s, s,
                                      t * 0.40f, t * 0.65f); // slow dual-axis spin
            // Restore the EXACT 2D HUD baseline (see the state set before the overlay
            // block). Critically, GL_TEXTURE_2D must be DISABLED with no texture bound,
            // otherwise the panel's colored quads sample the model texture and the whole
            // picker flickers black/golden as the selection changes.
            glViewport(0, 0, vw, vh);
            glUseProgram(0);
            glBindVertexArray(0);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
            for (int i = 0; i < 4; ++i) glDisableVertexAttribArray(i);
            glBindBufferBase(GL_UNIFORM_BUFFER, 0, 0);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, 0);
            glDisable(GL_TEXTURE_2D);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glDisable(GL_LIGHTING);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
          }
        }
      }

      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glColor4f(0.0f, 0.0f, 0.0f, 0.45f);
      glBegin(GL_QUADS); glVertex2i(px,0); glVertex2i(px+pw,0); glVertex2i(px+pw,vh); glVertex2i(px,vh); glEnd();
      glColor4f(1.0f, 0.85f, 0.0f, 0.7f);
      glBegin(GL_LINE_LOOP); glVertex2i(px,0); glVertex2i(px+pw,0); glVertex2i(px+pw,vh); glVertex2i(px,vh); glEnd();
      glDisable(GL_BLEND);

      draw_text(px + 8, 14, "Model IDs", 1.0f, 0.9f, 0.1f);

      // Filter box
      const int fbx1 = px + 4, fbx2 = px + pw - 4, fby1 = vh - hdr_h + 4, fby2 = vh - hdr_h + 22;
      glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glColor4f(0.1f, 0.1f, 0.15f, 0.9f);
      glBegin(GL_QUADS); glVertex2i(fbx1,fby1); glVertex2i(fbx2,fby1); glVertex2i(fbx2,fby2); glVertex2i(fbx1,fby2); glEnd();
      glColor4f(1.0f, 0.85f, 0.0f, 0.6f);
      glBegin(GL_LINE_LOOP); glVertex2i(fbx1,fby1); glVertex2i(fbx2,fby1); glVertex2i(fbx2,fby2); glVertex2i(fbx1,fby2); glEnd();
      glDisable(GL_BLEND);
      char fb[64]; snprintf(fb, sizeof(fb), "%s_", task_tree_view.model_picker_filter_.c_str());
      int fty = vh - (fby2) + 4;
      draw_text(fbx1 + 4, fty, fb, 1.0f, 1.0f, 1.0f);

      // Items
      int sel = task_tree_view.model_picker_selected_;
      int scroll = task_tree_view.model_picker_scroll_;
      for (int r = 0; r < max_vis; ++r) {
        int idx = scroll + r;
        if (idx >= count) break;
        int item_sy = hdr_h + r * row_h;
        if (idx == sel) {
          glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
          glColor4f(1.0f, 0.85f, 0.0f, 0.35f);
          glBegin(GL_QUADS);
          int gy1 = vh - item_sy - row_h, gy2 = vh - item_sy;
          glVertex2i(px,gy1); glVertex2i(px+pw,gy1); glVertex2i(px+pw,gy2); glVertex2i(px,gy2);
          glEnd(); glDisable(GL_BLEND);
          draw_text(px + 4, item_sy + 13, filtered[idx].label.c_str(), 1.0f, 0.9f, 0.1f);
        } else {
          draw_text(px + 4, item_sy + 13, filtered[idx].label.c_str(), 1.0f, 1.0f, 1.0f);
        }
      }
      draw_text(px + 4, vh - ftr_h + 4, "[Enter] Insert  [Esc] Cancel  [Type] Filter", 0.5f, 0.5f, 0.5f);
    }

    // ── In-Game HUD overlay (Authentic OpenIGI / Retail IGI StatusScreen) ──
    if (task_tree_view.in_game_mode_ && !task_tree_view.pause_mode_ &&
        task_tree_view.hud_overlay_visible_) {
      int vw = params.view_define_->viewport_width_;
      int vh = params.view_define_->viewport_height_;
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

      // ── 1. Tactical Reticle / Crosshair (Center) ─────────────────────────
      // OpenIGI Crosshair(): (w*0.5 - (int)(spriteW*0.5), h*0.5 - (int)(spriteH*0.5)).
      HudSprite cross_spr = GetOrLoadHudSprite("cross.spr");
      if (cross_spr.valid) {
        int cx = (int)(vw * 0.5f) - (int)(cross_spr.width  * 0.5f);
        int cy = (int)(vh * 0.5f) - (int)(cross_spr.height * 0.5f);
        DrawHudFrame(cross_spr, 0, cx, cy, 1.0f, 1.0f, 1.0f, 0.95f);
      } else {
        int cx = vw / 2, cy = vh / 2;
        glColor4f(0.0f, 1.0f, 0.2f, 0.9f);
        glLineWidth(1.5f);
        glBegin(GL_LINES);
        glVertex2i(cx - 10, cy); glVertex2i(cx - 3, cy);
        glVertex2i(cx + 3,  cy); glVertex2i(cx + 10, cy);
        glVertex2i(cx, cy - 10); glVertex2i(cx, cy - 3);
        glVertex2i(cx, cy + 3);  glVertex2i(cx, cy + 10);
        glEnd();
        glLineWidth(1.0f);
      }

      // ── 2. Authentic Health Powerbar (Bottom Left) ─────────────────────────
      // StatusScreenLayout: both sprites centred on (0.0625w, 0.89583331h from
      // the top); foreground inset 2px up-left; bottom-filled by remaining hp.
      const float maximum_health = std::max(0.0001f, task_tree_view.player_maximum_health_);
      const float maximum_armor = std::max(0.0001f, task_tree_view.player_maximum_armor_);
      float hp  = std::clamp(task_tree_view.player_health_, 0.0f, maximum_health);
      float arm = std::clamp(task_tree_view.player_armor_,  0.0f, maximum_armor);
      const float health_fraction = std::clamp(hp / maximum_health, 0.0f, 1.0f);

      HudSprite pb_bg  = GetOrLoadHudSprite("powerbarbackground.spr");
      HudSprite pb_fg  = GetOrLoadHudSprite("powerbar.spr");
      HudSprite hp_icon = GetOrLoadHudSprite("health.spr");

      const float kBaseline     = 0.89583331f;
      const int   kHealthInset  = 2;
      int anchor_x = (int)(vw * 0.0625f);
      int anchor_gl_y = (int)(vh * (1.0f - kBaseline));   // centre height above bottom

      if (pb_bg.valid && pb_fg.valid) {
        int bg_left = anchor_x - pb_bg.width / 2;
        int bg_top  = anchor_gl_y + pb_bg.height / 2;      // top edge in gl space
        DrawHudFrame(pb_bg, 0, bg_left, bg_top - pb_bg.height, 1.0f, 1.0f, 1.0f, 0.9f);

        // Foreground inset 2px up-left from the background top-left corner.
        int fg_left = bg_left - kHealthInset;
        int fg_top  = bg_top  + kHealthInset;
        DrawHudBarFill(pb_fg, fg_left, fg_top - pb_fg.height, health_fraction, 1.0f, 1.0f, 1.0f, 0.95f);

        // Health cross icon beside the bar (top-right of the background).
        if (hp_icon.valid) {
          int ix = bg_left + pb_bg.width + 6;
          DrawHudFrame(hp_icon, 0, ix, bg_top - hp_icon.height, 1.0f, 1.0f, 1.0f, 0.95f);
        }

        // Numeric HP & Armor readouts beside the icon.
        char hp_buf[32], arm_buf[32];
        snprintf(hp_buf, sizeof(hp_buf), "%d", (int)hp);
        snprintf(arm_buf, sizeof(arm_buf), "ARM %d", (int)arm);
        int text_x = bg_left + pb_bg.width + (hp_icon.valid ? (hp_icon.width + 12) : 10);
        draw_text_sys(text_x, vh - (bg_top - 2),                    hp_buf, 0.0f, 1.0f, 0.3f);
        draw_text_sys(text_x, vh - (bg_top - 22),                   arm_buf, 0.2f, 0.75f, 1.0f);
      } else {
        // High-fidelity fallback powerbar (same fractional anchor).
        const int BAR_W = 22, BAR_H = 80;
        int bx = (int)(vw * 0.0625f) - BAR_W / 2;
        int by = anchor_gl_y - BAR_H / 2;
        glColor4f(0.08f, 0.08f, 0.08f, 0.85f);
        glBegin(GL_QUADS);
        glVertex2i(bx, by); glVertex2i(bx + BAR_W, by);
        glVertex2i(bx + BAR_W, by + BAR_H); glVertex2i(bx, by + BAR_H);
        glEnd();

        int fill_h = (int)(BAR_H * health_fraction);
        if (health_fraction > 0.5f)      glColor4f(0.0f, 0.95f, 0.25f, 0.9f);
        else if (health_fraction > 0.25f) glColor4f(0.95f, 0.8f, 0.0f, 0.9f);
        else                 glColor4f(0.95f, 0.15f, 0.1f, 0.9f);
        glBegin(GL_QUADS);
        glVertex2i(bx + 2, by + 2); glVertex2i(bx + BAR_W - 2, by + 2);
        glVertex2i(bx + BAR_W - 2, by + 2 + fill_h); glVertex2i(bx + 2, by + 2 + fill_h);
        glEnd();

        glColor4f(0.0f, 0.8f, 0.2f, 0.8f);
        glBegin(GL_LINE_LOOP);
        glVertex2i(bx, by); glVertex2i(bx + BAR_W, by);
        glVertex2i(bx + BAR_W, by + BAR_H); glVertex2i(bx, by + BAR_H);
        glEnd();

        char hp_buf[32], arm_buf[32];
        snprintf(hp_buf, sizeof(hp_buf), "HP  %d", (int)hp);
        snprintf(arm_buf, sizeof(arm_buf), "ARM %d", (int)arm);
        draw_text_sys(bx + BAR_W + 12, vh - (by + BAR_H - 4), hp_buf, 0.0f, 1.0f, 0.3f);
        draw_text_sys(bx + BAR_W + 12, vh - (by + BAR_H - 24), arm_buf, 0.2f, 0.75f, 1.0f);
      }

      // ── 3. Authentic Weapon & Ammo Readout (Bottom Right) ───────────────────
      // Right edge at 0.984375*vw; readout baseline shared with the health bar.
      int right_m = (int)(vw * 0.984375f);
      int bline   = anchor_gl_y;   // GL-y of the weapon readout baseline
      int char_w  = (sysFontSize <= 11) ? 6 : (sysFontSize >= 15) ? 9 : 7;

      std::string wname = task_tree_view.active_weapon_name_;
      for (char& c : wname) c = (char)std::toupper((unsigned char)c);

      std::string wspr_name = GetWeaponSpriteName(wname);
      HudSprite wspr = GetOrLoadHudSprite(wspr_name);

      // Magazine gauge (clip20.spr): frame 0 when full, 60 when empty.
      HudSprite clip_spr = GetOrLoadHudSprite("clip20.spr");
      int clip_frame = 60;
      if (task_tree_view.clip_capacity_ > 0) {
        int rounds = (int)task_tree_view.clip_ammo_;
        if (rounds < 0) rounds = 0;
        clip_frame = 60 - (int)((60 * rounds) / (int)task_tree_view.clip_capacity_);
        if (clip_frame < 0) clip_frame = 0;
        if (clip_frame > 60) clip_frame = 60;
      }

      // Ammo count: right-aligned at right margin - AmmoInset(4), baseline 4px
      // below the shared readout baseline, "%-4d".
      char ammobuf[32];
      snprintf(ammobuf, sizeof(ammobuf), "%-4d", (int)task_tree_view.clip_ammo_);
      int ammoW = 4 * char_w;
      int ammo_x = right_m - 4 - ammoW;
      draw_text_sys(ammo_x, vh - (bline - 4), ammobuf, 0.0f, 1.0f, 0.4f);

      // Magazine gauge sits left of the ammo count with an 8px gap.
      if (clip_spr.valid) {
        int gx = ammo_x - 8 - clip_spr.width;
        int gy = (bline - 4) - clip_spr.height / 2 - sysLineH / 4;
        DrawHudFrame(clip_spr, clip_frame, gx, gy, 1.0f, 1.0f, 1.0f, 0.9f);
      }

      // Weapon name: right-aligned, baseline 2px above the shared baseline.
      int name_w = (int)wname.length() * char_w;
      int name_x = right_m - name_w;
      draw_text_sys(name_x, vh - (bline + 2), wname.c_str(), 0.0f, 1.0f, 0.3f);

      // Green underline below the name (right-aligned at the right margin).
      glColor4f(0.0f, 0.9f, 0.0f, 0.5f);
      glLineWidth(2.0f);
      glBegin(GL_LINES);
      glVertex2i(name_x - 3, bline);
      glVertex2i(right_m,    bline);
      glEnd();
      glLineWidth(1.0f);

      // Weapon icon above the name (right-aligned), 2px gap.
      if (wspr.valid) {
        int wy = bline + sysLineH + 2;                 // bottom of the icon
        DrawHudFrame(wspr, 0, right_m - wspr.width, wy, 1.0f, 1.0f, 1.0f, 0.95f);
      }

      // ── 4. Tactical Status / Objective Card (Top Left - StatusScreen style) ─
      if (!task_tree_view.objective_text_.empty()) {
        int ox = (int)(vw * 0.015625f);
        int oy = vh - 52;
        int pw = 280, ph = 38;

        glColor4f(0.02f, 0.10f, 0.03f, 0.80f);
        glBegin(GL_QUADS);
        glVertex2i(ox, oy); glVertex2i(ox + pw, oy);
        glVertex2i(ox + pw, oy + ph); glVertex2i(ox, oy + ph);
        glEnd();

        glColor4f(0.0f, 0.8f, 0.2f, 0.85f);
        glBegin(GL_LINE_LOOP);
        glVertex2i(ox, oy); glVertex2i(ox + pw, oy);
        glVertex2i(ox + pw, oy + ph); glVertex2i(ox, oy + ph);
        glEnd();

        draw_text_sys(ox + 8, 22, task_tree_view.objective_text_.c_str(), 0.0f, 0.95f, 0.25f);
      }

      // Authored StatusMessage tasks share the retail top-left message column.
      // The runtime has already applied the fixed-step typewriter state; this
      // pass only clips the immutable presentation text to the revealed count.
      const int status_left = (int)(vw * 0.015625f);
      const int status_top = 62;
      for (size_t message_index = 0;
           message_index < task_tree_view.mission_status_messages_.size();
           ++message_index) {
        const igi::MissionStatusMessageDisplay& message =
            task_tree_view.mission_status_messages_[message_index];
        const size_t visible_character_count = std::min<size_t>(
            message.revealed_characters,
            message.text.size());
        const std::string visible_text = message.text.substr(
            0,
            visible_character_count);
        const int card_top = status_top + static_cast<int>(message_index) * 28;
        const int card_bottom = card_top + 24;
        const int card_width = std::min(420, std::max(220,
            16 + static_cast<int>(visible_text.size()) * 8));
        glColor4f(0.02f, 0.10f, 0.03f, 0.82f);
        glBegin(GL_QUADS);
        glVertex2i(status_left, vh - card_bottom);
        glVertex2i(status_left + card_width, vh - card_bottom);
        glVertex2i(status_left + card_width, vh - card_top);
        glVertex2i(status_left, vh - card_top);
        glEnd();

        glColor4f(0.0f, 0.8f, 0.2f, 0.85f);
        glBegin(GL_LINE_LOOP);
        glVertex2i(status_left, vh - card_bottom);
        glVertex2i(status_left + card_width, vh - card_bottom);
        glVertex2i(status_left + card_width, vh - card_top);
        glVertex2i(status_left, vh - card_top);
        glEnd();
        draw_text_sys(
            status_left + 8,
            card_top + 5,
            visible_text.c_str(),
            0.0f,
            0.95f,
            0.25f);
      }

      if (task_tree_view.mission_timer_remaining_ticks_ >= 0) {
        const int total_seconds = static_cast<int>(
            task_tree_view.mission_timer_remaining_ticks_ / 30);
        char timer_text[32];
        snprintf(
            timer_text,
            sizeof(timer_text),
            "TIME %02d:%02d",
            total_seconds / 60,
            total_seconds % 60);
        draw_text_sys(vw - 108, 16, timer_text, 1.0f, 0.9f, 0.2f);
      }

      // The vanilla first-person muzzle cue is normally an authored weapon
      // sprite. Keep a small deterministic additive fallback so a firearm
      // still reads as firing when that sprite is not unpacked locally.
      const float muzzle_flash_alpha = std::clamp(
          task_tree_view.muzzle_flash_strength_, 0.0f, 1.0f);
      if (muzzle_flash_alpha > 0.0f) {
        const int flash_center_x = vw / 2 + std::max(18, vw / 12);
        const int flash_center_y = vh / 2 - std::max(12, vh / 10);
        const int outer_radius = std::max(16, std::min(vw, vh) / 18);
        const int inner_radius = std::max(5, outer_radius / 3);

        glDisable(GL_TEXTURE_2D);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glColor4f(1.0f, 0.55f, 0.08f, 0.70f * muzzle_flash_alpha);
        glBegin(GL_TRIANGLE_FAN);
        glVertex2i(flash_center_x, flash_center_y);
        glVertex2i(flash_center_x, flash_center_y + outer_radius);
        glVertex2i(flash_center_x + inner_radius, flash_center_y + inner_radius);
        glVertex2i(flash_center_x + outer_radius, flash_center_y);
        glVertex2i(flash_center_x + inner_radius, flash_center_y - inner_radius);
        glVertex2i(flash_center_x, flash_center_y - outer_radius);
        glVertex2i(flash_center_x - inner_radius, flash_center_y - inner_radius);
        glVertex2i(flash_center_x - outer_radius, flash_center_y);
        glVertex2i(flash_center_x - inner_radius, flash_center_y + inner_radius);
        glVertex2i(flash_center_x, flash_center_y + outer_radius);
        glEnd();

        glColor4f(1.0f, 0.95f, 0.70f, 0.85f * muzzle_flash_alpha);
        glBegin(GL_QUADS);
        glVertex2i(flash_center_x - inner_radius, flash_center_y - inner_radius);
        glVertex2i(flash_center_x + inner_radius, flash_center_y - inner_radius);
        glVertex2i(flash_center_x + inner_radius, flash_center_y + inner_radius);
        glVertex2i(flash_center_x - inner_radius, flash_center_y + inner_radius);
        glEnd();
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      }

      // Incoming fire needs an immediate readable cue even when the authored
      // damage sprite is unavailable. Keep it brief and translucent so the
      // health bar remains visible and the scene is not hidden behind a full
      // opaque flash.
      const float damage_alpha = std::clamp(
          task_tree_view.player_damage_effect_strength_, 0.0f, 1.0f);
      if (damage_alpha > 0.0f) {
        glColor4f(0.85f, 0.03f, 0.02f, 0.28f * damage_alpha);
        glBegin(GL_QUADS);
        glVertex2i(0, 0);
        glVertex2i(vw, 0);
        glVertex2i(vw, vh);
        glVertex2i(0, vh);
        glEnd();
      }

      // Flashbang exposure is drawn last so it washes out the weapon, HUD, and
      // scene together, matching the player-facing effect instead of merely
      // playing an audio cue.
      const float flash_alpha = std::clamp(
          task_tree_view.flash_effect_strength_, 0.0f, 1.0f);
      if (flash_alpha > 0.0f) {
        glColor4f(1.0f, 1.0f, 1.0f, flash_alpha);
        glBegin(GL_QUADS);
        glVertex2i(0, 0);
        glVertex2i(vw, 0);
        glVertex2i(vw, vh);
        glVertex2i(0, vh);
        glEnd();
      }

      // The retail map computer takes over the player-facing display. The
      // simulation has already copied the active objective rows into the
      // immutable snapshot; this pass only paints the phosphor-style panel.
      if (task_tree_view.map_computer_open_) {
        glColor4f(0.005f, 0.025f, 0.012f, 0.96f);
        glBegin(GL_QUADS);
        glVertex2i(0, 0);
        glVertex2i(vw, 0);
        glVertex2i(vw, vh);
        glVertex2i(0, vh);
        glEnd();

        const int panel_left = std::max(24, vw / 10);
        const int panel_right = std::min(vw - 24, (vw * 9) / 10);
        const int panel_top = std::max(36, vh / 10);
        const int panel_bottom = std::min(vh - 36, (vh * 9) / 10);
        glColor4f(0.0f, 0.75f, 0.18f, 0.80f);
        glBegin(GL_LINE_LOOP);
        glVertex2i(panel_left, panel_top);
        glVertex2i(panel_right, panel_top);
        glVertex2i(panel_right, panel_bottom);
        glVertex2i(panel_left, panel_bottom);
        glEnd();

        draw_text_sys(panel_left + 18, panel_top + 24,
                      "MAP COMPUTER", 0.0f, 1.0f, 0.30f);
        draw_text_sys(panel_left + 18, panel_top + 44,
                      "OBJECTIVES", 0.0f, 0.75f, 0.22f);

        // The map rows remain readable text, but the linked task locations
        // also need a spatial cue to make the tactical computer useful during
        // play. The simulation publishes immutable world positions; this
        // projection converts them to the current top-down map panel without
        // exposing mutable mission state to the renderer.
        const int map_left = panel_left + 18;
        const int map_top = panel_top + 64;
        const int map_right = panel_right - 18;
        const int map_bottom = panel_bottom - 64;
        const igi::RuntimeMapComputerProjection map_projection =
            igi::BuildRuntimeMapComputerProjection(
                params.view_define_->pos_,
                params.view_define_->fovy_,
                params.view_define_->viewport_width_,
                params.view_define_->viewport_height_,
                map_left,
                map_top,
                map_right,
                map_bottom);

        glColor4f(0.0f, 0.35f, 0.12f, 0.55f);
        glBegin(GL_LINES);
        for (int grid_line = 0; grid_line <= 8; ++grid_line) {
          const float horizontal_amount = static_cast<float>(grid_line) / 8.0f;
          const float x = static_cast<float>(map_left) +
              horizontal_amount * static_cast<float>(map_right - map_left);
          const float y = static_cast<float>(map_top) +
              horizontal_amount * static_cast<float>(map_bottom - map_top);
          glVertex2f(x, static_cast<float>(map_top));
          glVertex2f(x, static_cast<float>(map_bottom));
          glVertex2f(static_cast<float>(map_left), y);
          glVertex2f(static_cast<float>(map_right), y);
        }
        glEnd();

        const glm::vec2 player_marker = map_projection.Project(
            params.view_define_->pos_);
        glColor4f(0.2f, 1.0f, 0.35f, 0.95f);
        glBegin(GL_LINE_LOOP);
        glVertex2f(player_marker.x, player_marker.y + 8.0f);
        glVertex2f(player_marker.x + 8.0f, player_marker.y);
        glVertex2f(player_marker.x, player_marker.y - 8.0f);
        glVertex2f(player_marker.x - 8.0f, player_marker.y);
        glEnd();

        for (const igi::RuntimeMapComputerObjective& objective :
             task_tree_view.map_computer_objectives_) {
          if (!objective.has_location) {
            continue;
          }
          const glm::vec2 marker = map_projection.Project(objective.location);
          if (!map_projection.Contains(marker)) {
            continue;
          }

          if (objective.state == igi::ObjectiveState::Failed) {
            glColor4f(1.0f, 0.15f, 0.10f, 0.95f);
          } else if (objective.state == igi::ObjectiveState::Completed) {
            glColor4f(0.25f, 0.75f, 1.0f, 0.95f);
          } else {
            glColor4f(1.0f, 0.85f, 0.15f, 0.95f);
          }
          glBegin(GL_LINE_LOOP);
          glVertex2f(marker.x, marker.y + 6.0f);
          glVertex2f(marker.x + 6.0f, marker.y);
          glVertex2f(marker.x, marker.y - 6.0f);
          glVertex2f(marker.x - 6.0f, marker.y);
          glEnd();
        }

        for (size_t objective_index = 0;
             objective_index < task_tree_view.map_computer_objectives_.size();
             ++objective_index) {
          const igi::RuntimeMapComputerObjective& objective =
              task_tree_view.map_computer_objectives_[objective_index];
          const char* state_marker = "[ ]";
          if (objective.state == igi::ObjectiveState::Completed) {
            state_marker = "[X]";
          } else if (objective.state == igi::ObjectiveState::Failed) {
            state_marker = "[!]";
          } else if (objective.state == igi::ObjectiveState::Cancelled) {
            state_marker = "[-]";
          }

          const int row_y = panel_top + 78 +
              static_cast<int>(objective_index) * 24;
          const std::string row_text = std::to_string(objective_index + 1) +
              " " + state_marker + " " + objective.text;
          const float red = objective.state == igi::ObjectiveState::Failed
              ? 1.0f
              : 0.0f;
          const float green = objective.state == igi::ObjectiveState::Failed
              ? 0.25f
              : 0.95f;
          draw_text_sys(panel_left + 18, row_y, row_text.c_str(),
                        red, green, 0.25f);
        }

        draw_text_sys(panel_left + 18, panel_bottom - 22,
                      "[MAP COMPUTER] C TO CLOSE", 0.0f, 0.60f, 0.18f);
      }

      glDisable(GL_BLEND);
    }

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();

    // Restore all states
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glEnable(GL_LIGHTING);
    glEnable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
  }

  GL_CHECK_ERROR;

  glFlush();
}

// ---------------------------------------------------------------------------
// Navigation-graph overlay
// ---------------------------------------------------------------------------

// Load a single navigation graph for display. The caller maps the selected
// AIGraph task to graph<taskId>.dat. Read-only display.
bool Renderer::LoadGraphOverlayFile(const std::string& graphFilePath,
                                    const glm::dvec3& worldOffset) {
  graph_overlay_ = GRAPH_Parse(graphFilePath);
  graph_overlay_path_ = graphFilePath;
  graph_overlay_offset_ = worldOffset;
  graph_overlay_selected_ = -1;
  graph_link_source_ = -1;
  graph_overlay_dirty_ = false;

  if (graph_overlay_.valid && !graph_overlay_.nodes.empty()) {
    Logger::Get().Log(LogLevel::INFO,
        "[GRAPH] Overlay loaded " + std::to_string(graph_overlay_.nodes.size()) +
        " nodes, " + std::to_string(graph_overlay_.edges.size()) +
        " edges from: " + graphFilePath);
    return true;
  }
  Logger::Get().Log(LogLevel::INFO, "[GRAPH] Overlay: no usable graph at " + graphFilePath);
  return false;
}

bool Renderer::GetGraphNodePos(int id, glm::dvec3& out) const {
  const GraphNode* n = GRAPH_FindNode(graph_overlay_, id);
  if (!n) return false;
  // Return world position (file coords are local to the task graph origin).
  out = graph_overlay_offset_ + glm::dvec3(n->x, n->y, n->z);
  return true;
}

void Renderer::SetGraphNodePos(int id, const glm::dvec3& worldPos) {
  GraphNode* n = GRAPH_FindNode(graph_overlay_, id);
  if (!n) return;
  // Store back as local coords so the saved .dat stays in its native space.
  const glm::dvec3 local = worldPos - graph_overlay_offset_;
  n->x = local.x; n->y = local.y; n->z = local.z;
  graph_overlay_dirty_ = true;
}

bool Renderer::SaveGraphOverlay() {
  if (!graph_overlay_.valid || graph_overlay_path_.empty()) return false;
  // Full serialize so node moves, scales, creates and deletes all persist.
  if (!GRAPH_Write(graph_overlay_path_, graph_overlay_path_, graph_overlay_)) {
    Logger::Get().Log(LogLevel::ERR, "[GRAPH] Save failed: " + graph_overlay_path_);
    return false;
  }
  graph_overlay_dirty_ = false;
  Logger::Get().Log(LogLevel::INFO, "[GRAPH] Saved graph to: " + graph_overlay_path_);
  return true;
}

void Renderer::ScaleSelectedGraphNode(float factor) {
  GraphNode* n = GRAPH_FindNode(graph_overlay_, graph_overlay_selected_);
  if (!n) return;
  n->radius = (factor <= 0.0f) ? 1.0f : n->radius * factor;
  if (n->radius < 0.05f)  n->radius = 0.05f;
  if (n->radius > 100.0f) n->radius = 100.0f;
  graph_overlay_dirty_ = true;
}

int Renderer::CreateGraphNode() {
  if (!graph_overlay_.valid) return -1;
  int maxId = 0;
  for (const GraphNode& n : graph_overlay_.nodes) maxId = std::max(maxId, n.id);
  GraphNode nn;
  nn.id = maxId + 1;
  nn.radius = 1.0f;
  // Place near the current selection (offset a little), else near the first node.
  const GraphNode* anchor = GRAPH_FindNode(graph_overlay_, graph_overlay_selected_);
  if (!anchor && !graph_overlay_.nodes.empty()) anchor = &graph_overlay_.nodes.front();
  if (anchor) { nn.x = anchor->x + 500.0; nn.y = anchor->y + 500.0; nn.z = anchor->z; }
  graph_overlay_.nodes.push_back(nn);
  graph_overlay_selected_ = nn.id;
  graph_overlay_dirty_ = true;
  Logger::Get().Log(LogLevel::INFO, "[GRAPH] Created node " + std::to_string(nn.id));
  return nn.id;
}

void Renderer::DeleteSelectedGraphNode() {
  const int id = graph_overlay_selected_;
  if (id < 0) return;
  auto& ns = graph_overlay_.nodes;
  ns.erase(std::remove_if(ns.begin(), ns.end(),
           [id](const GraphNode& n) { return n.id == id; }), ns.end());
  auto& es = graph_overlay_.edges;
  es.erase(std::remove_if(es.begin(), es.end(),
           [id](const GraphEdge& e) { return e.node1 == id || e.node2 == id; }), es.end());
  graph_overlay_selected_ = -1;
  graph_overlay_dirty_ = true;
  Logger::Get().Log(LogLevel::INFO, "[GRAPH] Deleted node " + std::to_string(id));
}

std::string Renderer::AddGraphLinkStep() {
  if (!graph_overlay_.valid) return "";
  // First step: mark the selected node as the link source.
  if (graph_link_source_ < 0) {
    if (graph_overlay_selected_ < 0) return "Select a node first, then Alt++";
    graph_link_source_ = graph_overlay_selected_;
    return "Link source: node " + std::to_string(graph_link_source_) +
           " — select target, press Alt++ to link";
  }
  // Second step: link source to the currently selected node.
  const int src = graph_link_source_;
  const int dst = graph_overlay_selected_;
  graph_link_source_ = -1;
  if (dst < 0) return "Link cancelled (no target selected)";
  if (dst == src) return "Link cancelled (same node)";
  auto exists = std::find_if(graph_overlay_.edges.begin(), graph_overlay_.edges.end(),
      [&](const GraphEdge& e) {
        return (e.node1 == src && e.node2 == dst) || (e.node1 == dst && e.node2 == src);
      });
  if (exists != graph_overlay_.edges.end())
    return "Link " + std::to_string(src) + "-" + std::to_string(dst) + " already exists";
  GraphEdge e; e.node1 = src; e.node2 = dst; e.link_type = 1;
  graph_overlay_.edges.push_back(e);
  graph_overlay_dirty_ = true;
  Logger::Get().Log(LogLevel::INFO, "[GRAPH] Added link " +
      std::to_string(src) + "-" + std::to_string(dst));
  return "Link added " + std::to_string(src) + "-" + std::to_string(dst);
}

std::string Renderer::RemoveGraphLinkStep() {
  if (!graph_overlay_.valid) return "";
  if (graph_link_source_ < 0) {
    if (graph_overlay_selected_ < 0) return "Select a node first, then Alt+-";
    graph_link_source_ = graph_overlay_selected_;
    return "Link source: node " + std::to_string(graph_link_source_) +
           " — select target, press Alt+- to unlink";
  }
  const int src = graph_link_source_;
  const int dst = graph_overlay_selected_;
  graph_link_source_ = -1;
  if (dst < 0) return "Unlink cancelled (no target selected)";
  if (dst == src) return "Unlink cancelled (same node)";
  auto it = std::find_if(graph_overlay_.edges.begin(), graph_overlay_.edges.end(),
      [&](const GraphEdge& e) {
        return (e.node1 == src && e.node2 == dst) || (e.node1 == dst && e.node2 == src);
      });
  if (it == graph_overlay_.edges.end())
    return "No link between " + std::to_string(src) + "-" + std::to_string(dst);
  graph_overlay_.edges.erase(it);
  graph_overlay_dirty_ = true;
  Logger::Get().Log(LogLevel::INFO, "[GRAPH] Removed link " +
      std::to_string(src) + "-" + std::to_string(dst));
  return "Link removed " + std::to_string(src) + "-" + std::to_string(dst);
}

void Renderer::NudgeSelectedGraphNode(double dx, double dy, double dz) {
  GraphNode* n = GRAPH_FindNode(graph_overlay_, graph_overlay_selected_);
  if (!n) return;
  n->x += dx; n->y += dy; n->z += dz;
  graph_overlay_dirty_ = true;
}
void Renderer::AdjustSelectedGraphGamma(float d) {
  GraphNode* n = GRAPH_FindNode(graph_overlay_, graph_overlay_selected_);
  if (!n) return;
  n->gamma += d;
  graph_overlay_dirty_ = true;
}
void Renderer::AdjustSelectedGraphRadius(float d) {
  GraphNode* n = GRAPH_FindNode(graph_overlay_, graph_overlay_selected_);
  if (!n) return;
  n->radius += d;
  if (n->radius < 0.05f) n->radius = 0.05f;
  graph_overlay_dirty_ = true;
}
void Renderer::AdjustSelectedGraphMaterial(int d) {
  GraphNode* n = GRAPH_FindNode(graph_overlay_, graph_overlay_selected_);
  if (!n) return;
  n->material += d;
  if (n->material < 0)  n->material = 0;
  if (n->material > 23) n->material = 23;
  graph_overlay_dirty_ = true;
}
void Renderer::ToggleSelectedGraphCriteria(const std::string& key) {
  GraphNode* n = GRAPH_FindNode(graph_overlay_, graph_overlay_selected_);
  if (!n) return;
  if (n->criteria.find(key) != std::string::npos) n->criteria.clear();
  else n->criteria = "NODECRITERIA_" + key;
  graph_overlay_dirty_ = true;
}
bool Renderer::GetSelectedGraphNode(GraphNode& out) const {
  const GraphNode* n = GRAPH_FindNode(graph_overlay_, graph_overlay_selected_);
  if (!n) return false;
  out = *n;
  return true;
}

void Renderer::DrawGraphNodePanel(
    const draw_params_s& params,
    const std::function<void(int,int,const char*,float,float,float)>& draw_text_sm) {
  GraphNode node;
  if (!GetSelectedGraphNode(node)) return;

  const int vpH = params.view_define_->viewport_height_;
  const glm::dvec3 w = graph_overlay_offset_ + glm::dvec3(node.x, node.y, node.z);

  // Filled quad from a top-down rect (convert to GL bottom-up).
  auto quad = [&](int x, int y, int wd, int ht, float r, float g, float b, float a) {
    const float y0 = (float)(vpH - y), y1 = (float)(vpH - (y + ht));
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
    glVertex2f((float)x, y1); glVertex2f((float)(x + wd), y1);
    glVertex2f((float)(x + wd), y0); glVertex2f((float)x, y0);
    glEnd();
  };
  using namespace GraphNodePanel;
  glEnable(GL_BLEND);

  // Panel background + border (yellow theme — matches PropPanel).
  quad(PX, PY, PW, PanelHeight(), 0.04f, 0.04f, 0.05f, 0.78f);
  glColor4f(1.0f, 0.9f, 0.2f, 0.9f); glLineWidth(1.0f);
  glBegin(GL_LINE_LOOP);
  glVertex2f((float)PX, (float)(vpH - PY));
  glVertex2f((float)(PX + PW), (float)(vpH - PY));
  glVertex2f((float)(PX + PW), (float)(vpH - (PY + PanelHeight())));
  glVertex2f((float)PX, (float)(vpH - (PY + PanelHeight())));
  glEnd();

  char buf[96];
  snprintf(buf, sizeof(buf), "Graph Node  %d", node.id);
  draw_text_sm(PX + 8, PY + 6, buf, 1.0f, 0.9f, 0.2f);

  // A button helper: filled rect + centered-ish label (yellow theme).
  auto button = [&](int idx, const char* label, bool active) {
    int x, y, bw, bh; GetButtonRect(idx, x, y, bw, bh);
    if (active) quad(x, y, bw, bh, 0.85f, 0.72f, 0.08f, 0.95f);
    else        quad(x, y, bw, bh, 0.14f, 0.14f, 0.16f, 0.95f);
    draw_text_sm(x + 5, y + 4, label, 1.0f, 1.0f, 1.0f);
  };

  // Six numeric rows: label + value, with [-]/[+] buttons.
  const char* names[kNumericFields] = { "X", "Y", "Z", "Gamma", "Radius", "Mat" };
  double vals[kNumericFields] = { w.x, w.y, w.z, node.gamma, node.radius, (double)node.material };
  for (int f = 0; f < kNumericFields; ++f) {
    int rx, ry, rw, rh; GetButtonRect(kXDn + f * 2, rx, ry, rw, rh);
    if (f < 5) snprintf(buf, sizeof(buf), "%s: %.2f", names[f], vals[f]);
    else       snprintf(buf, sizeof(buf), "%s: %d", names[f], node.material);
    draw_text_sm(PX + 8, ry + 4, buf, 1.0f, 0.9f, 0.2f);
    button(kXDn + f * 2, "-", false);
    button(kXDn + f * 2 + 1, "+", false);
  }

  // Criteria toggles.
  draw_text_sm(PX + 8, GraphNodePanel::PY + HEADER_H + kNumericFields * ROW_H - 18, "Criteria:", 1.0f, 0.9f, 0.2f);
  button(kCrDoor,  "DOOR",  node.criteria.find("DOOR")  != std::string::npos);
  button(kCrView,  "VIEW",  node.criteria.find("VIEW")  != std::string::npos);
  button(kCrStair, "STAIR", node.criteria.find("STAIR") != std::string::npos);

  // Actions.
  button(kDelete, "Delete Node", false);
  button(kSave,   graph_overlay_dirty_ ? "Save Graph *" : "Save Graph", false);
}

int Renderer::PickGraphNodeAtScreen(int mx, int my, int vpW, int vpH) {
  if (!graph_overlay_.valid || graph_overlay_.nodes.empty()) return -1;
  // Same world-coord projection as the overlay draw (proj*view*scale on
  // offset+local), so picking lines up exactly with the rendered boxes.
  const glm::mat4 worldToClip =
      mat_proj_ * mat_view_ *
      glm::scale(glm::mat4(1.0f), glm::vec3(RENDERER_MODEL_SCALE_DOWN));
  const float baseH = (float)Config::Get().graphNodeSize * 100.0f;
  const float kMinThreshPx = 14.0f;
  int   bestId = -1;
  float bestD2 = std::numeric_limits<float>::max();
  for (const GraphNode& n : graph_overlay_.nodes) {
    const float H = baseH * std::max(1.0f, (float)n.radius);
    const glm::dvec3 w = graph_overlay_offset_ + glm::dvec3(n.x, n.y, n.z);
    const glm::vec4 clip = worldToClip * glm::vec4((float)w.x, (float)w.y, (float)w.z, 1.0f);
    if (clip.w <= 0.0f) continue;
    const float sx = (clip.x / clip.w * 0.5f + 0.5f) * vpW;
    const float sy = (-clip.y / clip.w * 0.5f + 0.5f) * vpH;
    // Project center+(H,0,0) to compute the node's screen-space half-extent so the
    // hit threshold matches the visible box size at any camera distance.
    float thresh = kMinThreshPx;
    const glm::vec4 eClip = worldToClip * glm::vec4((float)w.x + H, (float)w.y, (float)w.z, 1.0f);
    if (eClip.w > 0.0f) {
      const float ex = (eClip.x / eClip.w * 0.5f + 0.5f) * vpW;
      const float ey = (-eClip.y / eClip.w * 0.5f + 0.5f) * vpH;
      thresh = std::max(kMinThreshPx, std::sqrt((ex - sx) * (ex - sx) + (ey - sy) * (ey - sy)));
    }
    const float dx = sx - (float)mx, dy = sy - (float)my;
    const float d2 = dx * dx + dy * dy;
    if (d2 < thresh * thresh && d2 < bestD2) { bestD2 = d2; bestId = n.id; }
  }
  return bestId;
}

// Build the hover/selection info text for a node: id, criteria, world position,
// gamma/radius, and each link with its length (IGI units).
static std::string BuildGraphNodeInfo(const GraphFile& g, const glm::dvec3& offset, int id) {
  const GraphNode* n = GRAPH_FindNode(g, id);
  if (!n) return "";
  const glm::dvec3 w = offset + glm::dvec3(n->x, n->y, n->z);
  char buf[256];
  std::string s;
  snprintf(buf, sizeof(buf), "Node %d", id);                       s  = buf;
  s += "\nCriteria: " + (n->criteria.empty() ? std::string("(none)") : n->criteria);
  snprintf(buf, sizeof(buf), "\nX: %.1f  Y: %.1f  Z: %.1f", w.x, w.y, w.z); s += buf;
  snprintf(buf, sizeof(buf), "\nGamma: %.3f  Radius: %.3f", n->gamma, n->radius); s += buf;

  std::string links; int cnt = 0;
  for (const GraphEdge& e : g.edges) {
    int other = (e.node1 == id) ? e.node2 : (e.node2 == id) ? e.node1 : -1;
    if (other < 0) continue;
    const GraphNode* o = GRAPH_FindNode(g, other);
    if (!o) continue;
    const double dx = o->x - n->x, dy = o->y - n->y, dz = o->z - n->z;
    snprintf(buf, sizeof(buf), "\n  -> %d  (len %.0f)", other, std::sqrt(dx*dx + dy*dy + dz*dz));
    links += buf; ++cnt;
  }
  snprintf(buf, sizeof(buf), "\nLinks: %d", cnt); s += buf;
  s += links;
  return s;
}

void Renderer::DrawGraphOverlayInternal(
    const draw_params_s& params,
    const std::function<void(int,int,const char*,float,float,float)>& draw_text_sm,
    int mouseX, int mouseY) {
  if (!graph_overlay_.valid || graph_overlay_.nodes.empty()) return;

  const int vpW = params.view_define_->viewport_width_;
  const int vpH = params.view_define_->viewport_height_;

  // EXACT same transform the 3D scene/terrain uses (renderer.cpp mvp_objects_):
  // proj * view * scale applied to full WORLD coordinates. Node coords are local
  // to the task graph origin, so add the offset in double precision and project
  // the world point - this keeps the overlay locked to the world like real
  // geometry (no swimming/jitter relative to the terrain as the camera moves).
  const glm::mat4 worldToClip =
      mat_proj_ * mat_view_ *
      glm::scale(glm::mat4(1.0f), glm::vec3(RENDERER_MODEL_SCALE_DOWN));

  // Project a node LOCAL point to GL screen pixels (bottom-up) + window depth.
  auto project = [&](double lx, double ly, double lz, float& sx, float& sy, float& sz) -> bool {
    const glm::dvec3 w = graph_overlay_offset_ + glm::dvec3(lx, ly, lz);
    const glm::vec4 clip = worldToClip * glm::vec4((float)w.x, (float)w.y, (float)w.z, 1.0f);
    if (clip.w <= 0.0f) return false;
    sx = (clip.x / clip.w * 0.5f + 0.5f) * vpW;
    sy = (clip.y / clip.w * 0.5f + 0.5f) * vpH;
    sz = (clip.z / clip.w * 0.5f + 0.5f);  // window-space depth [0,1]
    return true;
  };

  // Node screen-space half-size (pixels) — used only for hover threshold and label offset.
  auto hsFor = [](float radius) -> float {
    float h = (radius > 0.01f ? radius : 0.5f) * 14.0f;
    return h < 5.0f ? 5.0f : (h > 30.0f ? 30.0f : h);
  };

  // Precompute screen position per node (in-front-of-camera check only; depth
  // occlusion is now handled by the GPU in the 3D node pass).
  struct NodeScreen { float sx, sy; bool vis; };
  const size_t NN = graph_overlay_.nodes.size();
  std::vector<NodeScreen> nsv(NN);
  std::unordered_map<int, int> idIdx;
  for (size_t i = 0; i < NN; ++i) {
    const GraphNode& n = graph_overlay_.nodes[i];
    idIdx[n.id] = static_cast<int>(i);
    float sx = 0, sy = 0, sz = 0;
    bool vis = project(n.x, n.y, n.z, sx, sy, sz);
    if (vis) {
      const int ix = (int)sx, iy = (int)sy;
      if (ix < 0 || iy < 0 || ix >= vpW || iy >= vpH) vis = false;
    }
    nsv[i] = { sx, sy, vis };
  }
  auto nsById = [&](int id) -> const NodeScreen* {
    auto it = idIdx.find(id); return it == idIdx.end() ? nullptr : &nsv[it->second];
  };

  // --- Hover detection (visible, non-occluded nodes only). ---
  int hoveredId = -1;
  {
    float bestD2 = 1e30f;
    for (size_t i = 0; i < NN; ++i) {
      if (!nsv[i].vis) continue;
      const GraphNode& n = graph_overlay_.nodes[i];
      const float dx = nsv[i].sx - (float)mouseX;
      const float dy = (vpH - nsv[i].sy) - (float)mouseY;
      const float d2 = dx * dx + dy * dy;
      const float thr = hsFor(n.radius) + 6.0f;
      if (d2 < thr * thr && d2 < bestD2) { bestD2 = d2; hoveredId = n.id; }
    }
  }
  const int activeId = (hoveredId >= 0) ? hoveredId : graph_overlay_selected_;

  glEnable(GL_BLEND);

  // Screen-space rings for selected (yellow) and hovered (white) nodes. The 3D
  // solid boxes are drawn in DrawGraphNodes3D (before the HUD); the rings are
  // intentionally in HUD space so they always show on top for quick identification.
  // Highlight rings for selected (yellow) and hovered (white), if visible.
  auto ring = [&](int id, float h, float r, float g, float b) {
    const NodeScreen* s = nsById(id);
    if (!s || !s->vis) return;
    glColor4f(r, g, b, 1.0f);
    glLineWidth(2.0f);
    glBegin(GL_LINE_LOOP);
    glVertex2f(s->sx - h, s->sy - h); glVertex2f(s->sx + h, s->sy - h);
    glVertex2f(s->sx + h, s->sy + h); glVertex2f(s->sx - h, s->sy + h);
    glEnd();
  };
  if (graph_overlay_selected_ >= 0) {
    const GraphNode* s = GRAPH_FindNode(graph_overlay_, graph_overlay_selected_);
    if (s) ring(graph_overlay_selected_, hsFor(s->radius) + 4.0f, 1.0f, 0.9f, 0.2f);
  }
  if (hoveredId >= 0) {
    const GraphNode* h = GRAPH_FindNode(graph_overlay_, hoveredId);
    if (h) ring(hoveredId, hsFor(h->radius) + 3.0f, 1.0f, 1.0f, 1.0f);
  }
  // Link source node (two-step link edit) — green dashed-style double ring.
  if (graph_link_source_ >= 0 && graph_link_source_ != graph_overlay_selected_) {
    const GraphNode* ls = GRAPH_FindNode(graph_overlay_, graph_link_source_);
    if (ls) {
      ring(graph_link_source_, hsFor(ls->radius) + 6.0f, 0.2f, 1.0f, 0.2f);
      ring(graph_link_source_, hsFor(ls->radius) + 9.0f, 0.2f, 1.0f, 0.2f);
    }
  }

  // --- Node id labels (visible nodes only, top-down y). ---
  if (graph_overlay_show_labels_) {
    for (size_t i = 0; i < NN; ++i) {
      if (!nsv[i].vis) continue;
      const GraphNode& n = graph_overlay_.nodes[i];
      char lbl[16]; snprintf(lbl, sizeof(lbl), "%d", n.id);
      draw_text_sm((int)nsv[i].sx + (int)hsFor(n.radius) + 2, vpH - (int)nsv[i].sy - 6, lbl, 1.0f, 1.0f, 1.0f);
    }
  }

  // --- Title banner: graph id + Area (from graph_level<N>.json) + counts. ---
  {
    char title[200];
    snprintf(title, sizeof(title), "Graph %s%s%s   (%zu nodes, %zu links)%s",
             graph_overlay_taskid_.empty() ? "?" : graph_overlay_taskid_.c_str(),
             graph_overlay_area_.empty() ? "" : " - ",
             graph_overlay_area_.c_str(),
             graph_overlay_.nodes.size(), graph_overlay_.edges.size(),
             graph_overlay_show_labels_ ? "" : "   [labels off]");
    draw_text_sm(360, 22, title, 1.0f, 0.85f, 0.4f);
  }

  // --- Info tooltip for the hovered node (or, if none, the selected node). ---
  if (activeId >= 0) {
    const std::string info = BuildGraphNodeInfo(graph_overlay_, graph_overlay_offset_, activeId);
    int lines = 1; for (char c : info) if (c == '\n') ++lines;

    int tx, ty;
    if (hoveredId >= 0) { tx = mouseX + 16; ty = mouseY + 16; }
    else {
      const NodeScreen* s = nsById(activeId);
      if (s) { tx = (int)s->sx + 12; ty = vpH - (int)s->sy + 8; }
      else { tx = 360; ty = 80; }
    }
    const int boxW = 230, boxH = lines * 14 + 8;
    if (tx + boxW > vpW) tx = vpW - boxW - 4;
    if (ty + boxH > vpH) ty = vpH - boxH - 4;

    // Dark background panel (top-down rect -> GL bottom-up quad).
    const float gx0 = (float)(tx - 6),       gx1 = (float)(tx + boxW);
    const float gy0 = (float)(vpH - (ty - 6)), gy1 = (float)(vpH - (ty + boxH));
    glColor4f(0.05f, 0.05f, 0.07f, 0.85f);
    glBegin(GL_QUADS);
    glVertex2f(gx0, gy1); glVertex2f(gx1, gy1); glVertex2f(gx1, gy0); glVertex2f(gx0, gy0);
    glEnd();
    glColor4f(1.0f, 0.9f, 0.2f, 0.9f);
    glLineWidth(1.0f);
    glBegin(GL_LINE_LOOP);
    glVertex2f(gx0, gy1); glVertex2f(gx1, gy1); glVertex2f(gx1, gy0); glVertex2f(gx0, gy0);
    glEnd();

    draw_text_sm(tx, ty, info.c_str(), 1.0f, 1.0f, 1.0f);
  }
}

void Renderer::DrawGraphNodes3D(const draw_params_s& params) {
  if (!graph_overlay_.valid || graph_overlay_.nodes.empty()) return;

  // Switch from object shader to fixed-function for 3D graph geometry.
  glUseProgram(0);
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  for (int i = 0; i < 4; ++i) glDisableVertexAttribArray(i);
  glBindBufferBase(GL_UNIFORM_BUFFER, 0, 0);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_TEXTURE_2D);
  glDisable(GL_LIGHTING);

  // Same world→clip transform as the 3D scene: proj * view * scale.
  glMatrixMode(GL_PROJECTION);
  glLoadMatrixf(glm::value_ptr(mat_proj_));
  glMatrixMode(GL_MODELVIEW);
  const glm::mat4 mv = mat_view_ *
      glm::scale(glm::mat4(1.0f), glm::vec3(RENDERER_MODEL_SCALE_DOWN));
  glLoadMatrixf(glm::value_ptr(mv));

  glEnable(GL_DEPTH_TEST);
  glDepthMask(GL_TRUE);
  glDepthFunc(GL_LEQUAL);
  glDisable(GL_CULL_FACE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  // Box half-extent in IGI world units, driven by QGraphNodeSize config and per-node radius.
  const float baseH = (float)Config::Get().graphNodeSize * 100.0f;

  // Draw solid cube faces with per-face shading (no GL lighting needed).
  glBegin(GL_QUADS);
  for (const GraphNode& n : graph_overlay_.nodes) {
    const float H = baseH * std::max(1.0f, (float)n.radius);
    const glm::dvec3 w = graph_overlay_offset_ + glm::dvec3(n.x, n.y, n.z);
    const float cx = (float)w.x, cy = (float)w.y;
    // Box sits entirely above the node's Z so it never clips into the ground.
    const float zBot = (float)w.z, zTop = zBot + H * 2.0f;

    float r, g, b;
    if (n.id == graph_overlay_selected_)      { r=1.0f;  g=0.6f;  b=0.0f;  }
    else switch (GRAPH_NodeKind(n)) {
      case GraphNodeKind::Door:  r=1.0f;  g=1.0f;  b=0.0f;  break;
      case GraphNodeKind::Stair: r=1.0f;  g=0.0f;  b=1.0f;  break;
      case GraphNodeKind::View:  r=0.0f;  g=1.0f;  b=1.0f;  break;
      default:                   r=0.85f; g=0.12f; b=0.12f; break;
    }

    // Top face (z+) — brightest
    glColor4f(r, g, b, 1.0f);
    glVertex3f(cx-H, cy-H, zTop); glVertex3f(cx+H, cy-H, zTop);
    glVertex3f(cx+H, cy+H, zTop); glVertex3f(cx-H, cy+H, zTop);

    // Front/back faces (y±) — medium
    glColor4f(r*0.72f, g*0.72f, b*0.72f, 1.0f);
    glVertex3f(cx-H, cy-H, zBot); glVertex3f(cx+H, cy-H, zBot);
    glVertex3f(cx+H, cy-H, zTop); glVertex3f(cx-H, cy-H, zTop);
    glVertex3f(cx+H, cy+H, zBot); glVertex3f(cx-H, cy+H, zBot);
    glVertex3f(cx-H, cy+H, zTop); glVertex3f(cx+H, cy+H, zTop);

    // Left/right faces (x±) — slightly darker
    glColor4f(r*0.58f, g*0.58f, b*0.58f, 1.0f);
    glVertex3f(cx-H, cy+H, zBot); glVertex3f(cx-H, cy-H, zBot);
    glVertex3f(cx-H, cy-H, zTop); glVertex3f(cx-H, cy+H, zTop);
    glVertex3f(cx+H, cy-H, zBot); glVertex3f(cx+H, cy+H, zBot);
    glVertex3f(cx+H, cy+H, zTop); glVertex3f(cx+H, cy-H, zTop);

    // Bottom face (z-) — darkest
    glColor4f(r*0.38f, g*0.38f, b*0.38f, 1.0f);
    glVertex3f(cx+H, cy-H, zBot); glVertex3f(cx-H, cy-H, zBot);
    glVertex3f(cx-H, cy+H, zBot); glVertex3f(cx+H, cy+H, zBot);
  }
  glEnd();

  // Wireframe outline for the selected node (yellow, slightly oversized).
  if (graph_overlay_selected_ >= 0) {
    const GraphNode* sn = GRAPH_FindNode(graph_overlay_, graph_overlay_selected_);
    if (sn) {
      const float H = baseH * std::max(1.0f, (float)sn->radius);
      const glm::dvec3 sw = graph_overlay_offset_ + glm::dvec3(sn->x, sn->y, sn->z);
      const float cx=(float)sw.x, cy=(float)sw.y, OH=H*1.08f;
      // Outline sits 8% oversized around the box; box runs [cz, cz+2H] so outline runs
      // [cz-(OH-H), cz+2H+(OH-H)] = [cz-0.08H, cz+2.08H].
      const float oBot = (float)sw.z - (OH - H);
      const float oTop = (float)sw.z + H*2.0f + (OH - H);
      glColor4f(1.0f, 0.9f, 0.2f, 1.0f);
      glLineWidth(2.5f);
      glBegin(GL_LINES);
      // Bottom ring (at ground level, slightly below box bottom)
      glVertex3f(cx-OH,cy-OH,oBot); glVertex3f(cx+OH,cy-OH,oBot);
      glVertex3f(cx+OH,cy-OH,oBot); glVertex3f(cx+OH,cy+OH,oBot);
      glVertex3f(cx+OH,cy+OH,oBot); glVertex3f(cx-OH,cy+OH,oBot);
      glVertex3f(cx-OH,cy+OH,oBot); glVertex3f(cx-OH,cy-OH,oBot);
      // Top ring
      glVertex3f(cx-OH,cy-OH,oTop); glVertex3f(cx+OH,cy-OH,oTop);
      glVertex3f(cx+OH,cy-OH,oTop); glVertex3f(cx+OH,cy+OH,oTop);
      glVertex3f(cx+OH,cy+OH,oTop); glVertex3f(cx-OH,cy+OH,oTop);
      glVertex3f(cx-OH,cy+OH,oTop); glVertex3f(cx-OH,cy-OH,oTop);
      // Verticals
      glVertex3f(cx-OH,cy-OH,oBot); glVertex3f(cx-OH,cy-OH,oTop);
      glVertex3f(cx+OH,cy-OH,oBot); glVertex3f(cx+OH,cy-OH,oTop);
      glVertex3f(cx+OH,cy+OH,oBot); glVertex3f(cx+OH,cy+OH,oTop);
      glVertex3f(cx-OH,cy+OH,oBot); glVertex3f(cx-OH,cy+OH,oTop);
      glEnd();
    }
  }

  // Edges as 3D lines between node centres.
  glLineWidth(1.5f);
  glBegin(GL_LINES);
  for (const GraphEdge& e : graph_overlay_.edges) {
    const GraphNode* na = GRAPH_FindNode(graph_overlay_, e.node1);
    const GraphNode* nb = GRAPH_FindNode(graph_overlay_, e.node2);
    if (!na || !nb) continue;
    const glm::dvec3 wa = graph_overlay_offset_ + glm::dvec3(na->x, na->y, na->z);
    const glm::dvec3 wb = graph_overlay_offset_ + glm::dvec3(nb->x, nb->y, nb->z);
    // Draw edges at the vertical centre of the node boxes (z + H) so they sit
    // above the ground and are clearly visible, not buried at ground level.
    const float Ha = baseH * std::max(1.0f, (float)na->radius);
    const float Hb = baseH * std::max(1.0f, (float)nb->radius);
    const bool active = graph_overlay_selected_ >= 0 &&
                        (e.node1 == graph_overlay_selected_ || e.node2 == graph_overlay_selected_);
    if (active) glColor4f(1.0f, 0.6f, 0.0f, 0.95f);
    else        glColor4f(0.72f, 0.72f, 0.72f, 0.55f);
    glVertex3f((float)wa.x, (float)wa.y, (float)wa.z + Ha);
    glVertex3f((float)wb.x, (float)wb.y, (float)wb.z + Hb);
  }
  glEnd();

  glLineWidth(1.0f);
  glDepthFunc(GL_LESS);
}
