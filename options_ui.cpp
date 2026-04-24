#include "options_ui.h"

#include "mat.h"
#include "render_internal.h"

#include <string>
#include <vector>

namespace {

// NDC layout — matches the challenge-select back-button rect so the
// "< Back" control sits in the same spot every user-facing screen.
constexpr float OPT_BACK_X  = -0.95f;
constexpr float OPT_BACK_Y  =  0.93f;
constexpr float OPT_BACK_W  =  0.20f;
constexpr float OPT_BACK_H  =  0.07f;

// Cartoon-outline toggle — a wide button that flips ON/OFF in place.
constexpr float OPT_TOG_W   =  0.60f;
constexpr float OPT_TOG_H   =  0.10f;
constexpr float OPT_TOG_X   = -OPT_TOG_W * 0.5f;
constexpr float OPT_TOG_Y   =  0.12f;

}  // namespace

int options_hit_test(double mx, double my, int width, int height) {
    float ndc_x = 2.0f * static_cast<float>(mx) / width - 1.0f;
    float ndc_y = 1.0f - 2.0f * static_cast<float>(my) / height;
    if (ndc_x >= OPT_BACK_X && ndc_x <= OPT_BACK_X + OPT_BACK_W &&
        ndc_y >= OPT_BACK_Y - OPT_BACK_H && ndc_y <= OPT_BACK_Y)
        return 1;
    if (ndc_x >= OPT_TOG_X && ndc_x <= OPT_TOG_X + OPT_TOG_W &&
        ndc_y >= OPT_TOG_Y - OPT_TOG_H && ndc_y <= OPT_TOG_Y)
        return 2;
    return 0;
}

void renderer_draw_options(bool cartoon_outline_enabled,
                           int width, int height,
                           int hover) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glViewport(0, 0, width, height);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    Mat4 id = mat4_identity();

    // --- Button backgrounds ---
    std::vector<float> bg;
    auto add_quad = [&](float x, float y, float w, float h) {
        bg.insert(bg.end(), {x, y-h, 0,  x+w, y-h, 0,  x+w, y, 0,
                              x, y-h, 0,  x+w, y, 0,  x, y, 0});
    };
    add_quad(OPT_BACK_X, OPT_BACK_Y, OPT_BACK_W, OPT_BACK_H);
    add_quad(OPT_TOG_X,  OPT_TOG_Y,  OPT_TOG_W,  OPT_TOG_H);

    GLuint bvao, bvbo;
    glGenVertexArrays(1, &bvao); glGenBuffers(1, &bvbo);
    glBindVertexArray(bvao); glBindBuffer(GL_ARRAY_BUFFER, bvbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bg.size() * sizeof(float)),
                 bg.data(), GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glUseProgram(g_highlight_program);
    glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"), 1, GL_FALSE, id.m);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);
    glUniform1i(glGetUniformLocation(g_highlight_program, "uUseGradient"), 0);

    glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                0.4f, 0.4f, 0.4f, hover == 1 ? 0.6f : 0.4f);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Toggle tints green when ON, grey when OFF; brighter on hover.
    if (cartoon_outline_enabled) {
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                    0.22f, 0.60f, 0.30f, hover == 2 ? 0.75f : 0.55f);
    } else {
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                    0.28f, 0.30f, 0.36f, hover == 2 ? 0.75f : 0.55f);
    }
    glDrawArrays(GL_TRIANGLES, 6, 6);
    glBindVertexArray(0); glDeleteBuffers(1, &bvbo); glDeleteVertexArrays(1, &bvao);

    // --- Text ---
    std::vector<float> text_verts;

    float tcw = 0.05f, tch = 0.075f;
    std::string title = "Options";
    float tw = title.size() * tcw * 0.7f;
    add_screen_string(text_verts, -tw * 0.5f, 0.6f, tcw, tch, title);
    int title_count = static_cast<int>(text_verts.size() / 5);

    float bw_cw = 0.022f, bw_ch = 0.032f;
    std::string back_text = "< Back";
    add_screen_string(text_verts, OPT_BACK_X + 0.04f,
                      OPT_BACK_Y - 0.020f, bw_cw, bw_ch, back_text);
    int back_end = static_cast<int>(text_verts.size() / 5);

    // Label on the toggle — "Cartoon outline: ON / OFF"
    float lcw = 0.028f, lch = 0.042f;
    std::string toggle_text =
        std::string("Cartoon outline: ") +
        (cartoon_outline_enabled ? "ON" : "OFF");
    float lw = toggle_text.size() * lcw * 0.7f;
    add_screen_string(text_verts, -lw * 0.5f,
                      OPT_TOG_Y - (OPT_TOG_H - lch) * 0.5f - 0.005f,
                      lcw, lch, toggle_text);
    int toggle_end = static_cast<int>(text_verts.size() / 5);

    GLuint tvao, tvbo;
    glGenVertexArrays(1, &tvao); glGenBuffers(1, &tvbo);
    glBindVertexArray(tvao); glBindBuffer(GL_ARRAY_BUFFER, tvbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(text_verts.size() * sizeof(float)),
                 text_verts.data(), GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(1);

    glUseProgram(g_text_program);
    glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"), 1, GL_FALSE, id.m);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_font_tex);
    glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);

    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 1.0f, 0.9f, 0.6f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, title_count);

    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 0.85f, 0.85f, 0.85f, 1.0f);
    glDrawArrays(GL_TRIANGLES, title_count, back_end - title_count);

    float li = hover == 2 ? 1.0f : 0.92f;
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), li, li, li, 1.0f);
    glDrawArrays(GL_TRIANGLES, back_end, toggle_end - back_end);

    glBindVertexArray(0); glDeleteBuffers(1, &tvbo); glDeleteVertexArrays(1, &tvao);

    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}
