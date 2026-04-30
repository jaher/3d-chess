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

// Continuous-voice toggle — same width, sits directly below with a
// small vertical gap.
constexpr float OPT_TOG2_W  =  0.60f;
constexpr float OPT_TOG2_H  =  0.10f;
constexpr float OPT_TOG2_X  = -OPT_TOG2_W * 0.5f;
constexpr float OPT_TOG2_Y  = -0.02f;

// Chessnut Move toggle — third row.
constexpr float OPT_TOG3_W  =  0.60f;
constexpr float OPT_TOG3_H  =  0.10f;
constexpr float OPT_TOG3_X  = -OPT_TOG3_W * 0.5f;
constexpr float OPT_TOG3_Y  = -0.16f;

}  // namespace

int options_hit_test(double mx, double my, int width, int height,
                     bool continuous_voice_supported,
                     bool chessnut_supported) {
    float ndc_x = 2.0f * static_cast<float>(mx) / width - 1.0f;
    float ndc_y = 1.0f - 2.0f * static_cast<float>(my) / height;
    if (ndc_x >= OPT_BACK_X && ndc_x <= OPT_BACK_X + OPT_BACK_W &&
        ndc_y >= OPT_BACK_Y - OPT_BACK_H && ndc_y <= OPT_BACK_Y)
        return 1;
    if (ndc_x >= OPT_TOG_X && ndc_x <= OPT_TOG_X + OPT_TOG_W &&
        ndc_y >= OPT_TOG_Y - OPT_TOG_H && ndc_y <= OPT_TOG_Y)
        return 2;
    if (continuous_voice_supported &&
        ndc_x >= OPT_TOG2_X && ndc_x <= OPT_TOG2_X + OPT_TOG2_W &&
        ndc_y >= OPT_TOG2_Y - OPT_TOG2_H && ndc_y <= OPT_TOG2_Y)
        return 3;
    if (chessnut_supported &&
        ndc_x >= OPT_TOG3_X && ndc_x <= OPT_TOG3_X + OPT_TOG3_W &&
        ndc_y >= OPT_TOG3_Y - OPT_TOG3_H && ndc_y <= OPT_TOG3_Y)
        return 4;
    return 0;
}

void renderer_draw_options(bool cartoon_outline_enabled,
                           bool voice_continuous_enabled,
                           bool continuous_voice_supported,
                           bool chessnut_enabled,
                           bool chessnut_supported,
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
    if (continuous_voice_supported) {
        add_quad(OPT_TOG2_X, OPT_TOG2_Y, OPT_TOG2_W, OPT_TOG2_H);
    }
    if (chessnut_supported) {
        add_quad(OPT_TOG3_X, OPT_TOG3_Y, OPT_TOG3_W, OPT_TOG3_H);
    }

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
    auto draw_toggle = [&](bool on, int hover_id, int vert_offset) {
        if (on) {
            glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                        0.22f, 0.60f, 0.30f,
                        hover == hover_id ? 0.75f : 0.55f);
        } else {
            glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                        0.28f, 0.30f, 0.36f,
                        hover == hover_id ? 0.75f : 0.55f);
        }
        glDrawArrays(GL_TRIANGLES, vert_offset, 6);
    };
    draw_toggle(cartoon_outline_enabled, 2, 6);
    int next_offset = 12;
    if (continuous_voice_supported) {
        draw_toggle(voice_continuous_enabled, 3, next_offset);
        next_offset += 6;
    }
    if (chessnut_supported) {
        draw_toggle(chessnut_enabled, 4, next_offset);
    }
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

    // Toggle labels. Same character metrics as the existing row.
    float lcw = 0.028f, lch = 0.042f;
    auto add_toggle_label = [&](const std::string& label, float row_y) {
        float lw = label.size() * lcw * 0.7f;
        add_screen_string(text_verts, -lw * 0.5f,
                          row_y - (OPT_TOG_H - lch) * 0.5f - 0.005f,
                          lcw, lch, label);
    };
    add_toggle_label(
        std::string("Cartoon outline: ") +
            (cartoon_outline_enabled ? "ON" : "OFF"),
        OPT_TOG_Y);
    int toggle_end = static_cast<int>(text_verts.size() / 5);
    int toggle2_end = toggle_end;
    if (continuous_voice_supported) {
        add_toggle_label(
            std::string("Continuous voice: ") +
                (voice_continuous_enabled ? "ON" : "OFF"),
            OPT_TOG2_Y);
        toggle2_end = static_cast<int>(text_verts.size() / 5);
    }
    int toggle3_end = toggle2_end;
    if (chessnut_supported) {
        add_toggle_label(
            std::string("Chessnut Move: ") +
                (chessnut_enabled ? "ON" : "OFF"),
            OPT_TOG3_Y);
        toggle3_end = static_cast<int>(text_verts.size() / 5);
    }

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

    float li1 = hover == 2 ? 1.0f : 0.92f;
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), li1, li1, li1, 1.0f);
    glDrawArrays(GL_TRIANGLES, back_end, toggle_end - back_end);

    if (continuous_voice_supported && toggle2_end > toggle_end) {
        float li2 = hover == 3 ? 1.0f : 0.92f;
        glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                    li2, li2, li2, 1.0f);
        glDrawArrays(GL_TRIANGLES, toggle_end, toggle2_end - toggle_end);
    }
    if (chessnut_supported && toggle3_end > toggle2_end) {
        float li3 = hover == 4 ? 1.0f : 0.92f;
        glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                    li3, li3, li3, 1.0f);
        glDrawArrays(GL_TRIANGLES, toggle2_end, toggle3_end - toggle2_end);
    }

    glBindVertexArray(0); glDeleteBuffers(1, &tvbo); glDeleteVertexArrays(1, &tvao);

    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}
