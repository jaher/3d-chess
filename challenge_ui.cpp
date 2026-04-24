#include "challenge_ui.h"

#include "mat.h"
#include "render_internal.h"

#include <algorithm>
#include <cstdio>

// ===========================================================================
// Challenge select screen
// ===========================================================================
namespace {

constexpr float CS_BTN_H     = 0.08f;
constexpr float CS_BTN_PAD   = 0.05f;  // horizontal padding each side
constexpr float CS_BTN_MIN_W = 0.3f;   // short names still look like buttons
constexpr float CS_TOP_Y     = 0.4f;
constexpr float CS_GAP       = 0.02f;
constexpr float CS_BACK_W    = 0.2f;
constexpr float CS_BACK_H    = 0.07f;
constexpr float CS_BACK_X    = -0.95f;
constexpr float CS_BACK_Y    = 0.93f;

// Glyph cell size used for challenge-name labels; both the button
// sizing and the label-draw path read from these so they stay in sync.
constexpr float CS_NAME_CW = 0.024f;
constexpr float CS_NAME_CH = 0.036f;

// Width needed to hold a challenge name's label, matching the
// add_screen_string width used below (0.7 × char width per glyph).
float cs_button_width_for(const std::string& name) {
    float text_w = static_cast<float>(name.size()) * CS_NAME_CW * 0.7f;
    float w = text_w + 2.0f * CS_BTN_PAD;
    if (w < CS_BTN_MIN_W) w = CS_BTN_MIN_W;
    return w;
}

}  // namespace

int challenge_select_hit_test(double mx, double my, int width, int height,
                              const std::vector<std::string>& challenge_names) {
    float ndc_x = 2.0f * static_cast<float>(mx) / width - 1.0f;
    float ndc_y = 1.0f - 2.0f * static_cast<float>(my) / height;

    if (ndc_x >= CS_BACK_X && ndc_x <= CS_BACK_X + CS_BACK_W &&
        ndc_y >= CS_BACK_Y - CS_BACK_H && ndc_y <= CS_BACK_Y)
        return -2;

    // Challenge buttons: each is centered on x=0, width fits its label.
    for (int i = 0; i < static_cast<int>(challenge_names.size()); i++) {
        float by = CS_TOP_Y - i * (CS_BTN_H + CS_GAP);
        float bw = cs_button_width_for(challenge_names[i]);
        float bx = -bw * 0.5f;
        if (ndc_x >= bx && ndc_x <= bx + bw &&
            ndc_y >= by - CS_BTN_H && ndc_y <= by)
            return i;
    }
    return -1;
}

void renderer_draw_challenge_select(const std::vector<std::string>& challenge_names,
                                    int width, int height, int hover_index) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glViewport(0, 0, width, height);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    Mat4 id = mat4_identity();

    std::vector<float> bg_verts;
    auto add_quad = [&](float x, float y, float w, float h) {
        bg_verts.insert(bg_verts.end(), {x, y-h, 0,  x+w, y-h, 0,  x+w, y, 0,
                                          x, y-h, 0,  x+w, y, 0,  x, y, 0});
    };
    add_quad(CS_BACK_X, CS_BACK_Y, CS_BACK_W, CS_BACK_H);
    for (int i = 0; i < static_cast<int>(challenge_names.size()); i++) {
        float by = CS_TOP_Y - i * (CS_BTN_H + CS_GAP);
        float bw = cs_button_width_for(challenge_names[i]);
        add_quad(-bw * 0.5f, by, bw, CS_BTN_H);
    }

    GLuint bvao, bvbo;
    glGenVertexArrays(1, &bvao); glGenBuffers(1, &bvbo);
    glBindVertexArray(bvao); glBindBuffer(GL_ARRAY_BUFFER, bvbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bg_verts.size()*sizeof(float)),
                 bg_verts.data(), GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glUseProgram(g_highlight_program);
    glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"), 1, GL_FALSE, id.m);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);

    glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                0.4f, 0.4f, 0.4f, hover_index == -2 ? 0.6f : 0.4f);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    for (int i = 0; i < static_cast<int>(challenge_names.size()); i++) {
        bool h = (hover_index == i);
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                    0.2f, 0.6f, 0.3f, h ? 0.6f : 0.35f);
        glDrawArrays(GL_TRIANGLES, 6 + i*6, 6);
    }
    glBindVertexArray(0); glDeleteBuffers(1, &bvbo); glDeleteVertexArrays(1, &bvao);

    std::vector<float> text_verts;

    float tcw = 0.05f, tch = 0.075f;
    std::string title = "Select Challenge";
    float tw = title.size() * tcw * 0.7f;
    add_screen_string(text_verts, -tw*0.5f, 0.6f, tcw, tch, title);
    int title_count = static_cast<int>(text_verts.size() / 5);

    float bw_cw = 0.022f, bw_ch = 0.032f;
    std::string back_text = "< Back";
    add_screen_string(text_verts, CS_BACK_X + 0.04f, CS_BACK_Y - 0.020f, bw_cw, bw_ch, back_text);
    int back_end = static_cast<int>(text_verts.size() / 5);

    // Challenge names — cell size matches CS_NAME_CW/CH so the button
    // sizing helper above produces labels that fit inside their button.
    std::vector<int> name_ends;
    for (int i = 0; i < static_cast<int>(challenge_names.size()); i++) {
        float by = CS_TOP_Y - i * (CS_BTN_H + CS_GAP);
        float nw = challenge_names[i].size() * CS_NAME_CW * 0.7f;
        add_screen_string(text_verts, -nw*0.5f, by - 0.025f,
                          CS_NAME_CW, CS_NAME_CH, challenge_names[i]);
        name_ends.push_back(static_cast<int>(text_verts.size() / 5));
    }

    if (text_verts.empty()) {
        glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
        return;
    }

    GLuint tvao, tvbo;
    glGenVertexArrays(1, &tvao); glGenBuffers(1, &tvbo);
    glBindVertexArray(tvao); glBindBuffer(GL_ARRAY_BUFFER, tvbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(text_verts.size()*sizeof(float)),
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

    int prev = back_end;
    for (int i = 0; i < static_cast<int>(name_ends.size()); i++) {
        float bri = (hover_index == i) ? 1.0f : 0.85f;
        glUniform4f(glGetUniformLocation(g_text_program, "uColor"), bri, bri, bri, 1.0f);
        glDrawArrays(GL_TRIANGLES, prev, name_ends[i] - prev);
        prev = name_ends[i];
    }

    glBindVertexArray(0);
    glDeleteBuffers(1, &tvbo); glDeleteVertexArrays(1, &tvao);

    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}

// ===========================================================================
// Challenge in-game overlay (drawn on top of regular game render)
// ===========================================================================
void renderer_draw_challenge_overlay(const std::string& challenge_name,
                                     int puzzle_index, int total_puzzles,
                                     int moves_made, int max_moves,
                                     bool starts_white,
                                     int /*width*/, int /*height*/) {
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    Mat4 id = mat4_identity();

    // Top-center info bar. Width is just enough to enclose the
    // longest realistic "Puzzle N/M   Colour to mate in N   Moves:
    // N/M" string (~0.58 NDC wide at the current font) with a small
    // padding. A wider bar would overlap the score graph at x = 0.55.
    std::vector<float> bg_verts;
    bg_verts.insert(bg_verts.end(), {-0.36f, 0.85f, 0,  0.36f, 0.85f, 0,  0.36f, 0.97f, 0,
                                       -0.36f, 0.85f, 0,  0.36f, 0.97f, 0,  -0.36f, 0.97f, 0});
    GLuint bvao, bvbo;
    glGenVertexArrays(1, &bvao); glGenBuffers(1, &bvbo);
    glBindVertexArray(bvao); glBindBuffer(GL_ARRAY_BUFFER, bvbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float)*bg_verts.size(), bg_verts.data(), GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glUseProgram(g_highlight_program);
    glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"), 1, GL_FALSE, id.m);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);
    glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), 0, 0, 0, 0.6f);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0); glDeleteBuffers(1, &bvbo); glDeleteVertexArrays(1, &bvao);

    std::vector<float> text_verts;
    float cw = 0.018f, ch = 0.026f;

    std::string line1 = challenge_name;
    float w1 = line1.size() * cw * 0.7f;
    add_screen_string(text_verts, -w1*0.5f, 0.945f, cw, ch, line1);
    int line1_end = static_cast<int>(text_verts.size() / 5);

    char buf[128];
    std::snprintf(buf, sizeof(buf), "Puzzle %d/%d   %s to mate in %d   Moves: %d/%d",
                  puzzle_index + 1, total_puzzles,
                  starts_white ? "White" : "Black",
                  max_moves, moves_made, max_moves);
    std::string line2 = buf;
    float w2 = line2.size() * cw * 0.7f;
    add_screen_string(text_verts, -w2*0.5f, 0.895f, cw, ch, line2);
    int line2_end = static_cast<int>(text_verts.size() / 5);

    std::string line3 = "ESC: reset   M: menu";
    float cw3 = 0.014f, ch3 = 0.020f;
    float w3 = line3.size() * cw3 * 0.7f;
    add_screen_string(text_verts, -w3*0.5f, -0.92f, cw3, ch3, line3);
    int line3_end = static_cast<int>(text_verts.size() / 5);

    GLuint tvao, tvbo;
    glGenVertexArrays(1, &tvao); glGenBuffers(1, &tvbo);
    glBindVertexArray(tvao); glBindBuffer(GL_ARRAY_BUFFER, tvbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float)*text_verts.size(), text_verts.data(), GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(1);

    glUseProgram(g_text_program);
    glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"), 1, GL_FALSE, id.m);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_font_tex);
    glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 1.0f, 0.9f, 0.5f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, line1_end);
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 0.9f, 0.9f, 0.9f, 1.0f);
    glDrawArrays(GL_TRIANGLES, line1_end, line2_end - line1_end);
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 0.7f, 0.7f, 0.7f, 0.8f);
    glDrawArrays(GL_TRIANGLES, line2_end, line3_end - line2_end);

    glBindVertexArray(0);
    glDeleteBuffers(1, &tvbo); glDeleteVertexArrays(1, &tvao);

    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}

// ===========================================================================
// Next-puzzle button
// ===========================================================================
namespace {
constexpr float NEXT_BTN_W = 0.3f;
constexpr float NEXT_BTN_H = 0.10f;
constexpr float NEXT_BTN_X = -NEXT_BTN_W * 0.5f;
constexpr float NEXT_BTN_Y = -0.20f;
}  // namespace

bool next_button_hit_test(double mx, double my, int width, int height) {
    float ndc_x = 2.0f * static_cast<float>(mx) / width - 1.0f;
    float ndc_y = 1.0f - 2.0f * static_cast<float>(my) / height;
    return ndc_x >= NEXT_BTN_X && ndc_x <= NEXT_BTN_X + NEXT_BTN_W &&
           ndc_y >= NEXT_BTN_Y - NEXT_BTN_H && ndc_y <= NEXT_BTN_Y;
}

void renderer_draw_next_button(int /*width*/, int /*height*/, bool hover) {
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    Mat4 id = mat4_identity();

    std::vector<float> text_verts;
    float lcw = 0.045f, lch = 0.065f;
    std::string label = "Solved!";
    float lw = label.size() * lcw * 0.7f;
    add_screen_string(text_verts, -lw * 0.5f, 0.05f, lcw, lch, label);
    int label_count = static_cast<int>(text_verts.size() / 5);

    float bcw = 0.030f, bch = 0.045f;
    std::string btn_text = "Next >";
    float btw = btn_text.size() * bcw * 0.7f;
    add_screen_string(text_verts, -btw * 0.5f, NEXT_BTN_Y - 0.022f, bcw, bch, btn_text);
    int total_count = static_cast<int>(text_verts.size() / 5);

    float bg[] = {
        NEXT_BTN_X, NEXT_BTN_Y - NEXT_BTN_H, 0,
        NEXT_BTN_X + NEXT_BTN_W, NEXT_BTN_Y - NEXT_BTN_H, 0,
        NEXT_BTN_X + NEXT_BTN_W, NEXT_BTN_Y, 0,
        NEXT_BTN_X, NEXT_BTN_Y - NEXT_BTN_H, 0,
        NEXT_BTN_X + NEXT_BTN_W, NEXT_BTN_Y, 0,
        NEXT_BTN_X, NEXT_BTN_Y, 0
    };
    GLuint bvao, bvbo;
    glGenVertexArrays(1, &bvao); glGenBuffers(1, &bvbo);
    glBindVertexArray(bvao); glBindBuffer(GL_ARRAY_BUFFER, bvbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(bg), bg, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glUseProgram(g_highlight_program);
    glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"), 1, GL_FALSE, id.m);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);
    glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                0.2f, 0.7f, 0.3f, hover ? 0.85f : 0.65f);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0); glDeleteBuffers(1, &bvbo); glDeleteVertexArrays(1, &bvao);

    GLuint tvao, tvbo;
    glGenVertexArrays(1, &tvao); glGenBuffers(1, &tvbo);
    glBindVertexArray(tvao); glBindBuffer(GL_ARRAY_BUFFER, tvbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(text_verts.size()*sizeof(float)),
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

    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 1.0f, 0.9f, 0.4f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, label_count);

    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 1, 1, 1, 1);
    glDrawArrays(GL_TRIANGLES, label_count, total_count - label_count);

    glBindVertexArray(0); glDeleteBuffers(1, &tvbo); glDeleteVertexArrays(1, &tvao);

    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}

// ===========================================================================
// Try-again button (drawn on a mate-in-N mistake)
// ===========================================================================
namespace {
constexpr float TRY_BTN_W = 0.36f;
constexpr float TRY_BTN_H = 0.10f;
constexpr float TRY_BTN_X = -TRY_BTN_W * 0.5f;
constexpr float TRY_BTN_Y = -0.20f;
}  // namespace

bool try_again_button_hit_test(double mx, double my, int width, int height) {
    float ndc_x = 2.0f * static_cast<float>(mx) / width - 1.0f;
    float ndc_y = 1.0f - 2.0f * static_cast<float>(my) / height;
    return ndc_x >= TRY_BTN_X && ndc_x <= TRY_BTN_X + TRY_BTN_W &&
           ndc_y >= TRY_BTN_Y - TRY_BTN_H && ndc_y <= TRY_BTN_Y;
}

void renderer_draw_try_again_button(int /*width*/, int /*height*/, bool hover) {
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    Mat4 id = mat4_identity();

    std::vector<float> text_verts;
    float lcw = 0.045f, lch = 0.065f;
    std::string label = "Mistake!";
    float lw = label.size() * lcw * 0.7f;
    add_screen_string(text_verts, -lw * 0.5f, 0.05f, lcw, lch, label);
    int label_count = static_cast<int>(text_verts.size() / 5);

    float bcw = 0.030f, bch = 0.045f;
    std::string btn_text = "Try Again";
    float btw = btn_text.size() * bcw * 0.7f;
    add_screen_string(text_verts, -btw * 0.5f, TRY_BTN_Y - 0.022f, bcw, bch, btn_text);
    int total_count = static_cast<int>(text_verts.size() / 5);

    float bg[] = {
        TRY_BTN_X, TRY_BTN_Y - TRY_BTN_H, 0,
        TRY_BTN_X + TRY_BTN_W, TRY_BTN_Y - TRY_BTN_H, 0,
        TRY_BTN_X + TRY_BTN_W, TRY_BTN_Y, 0,
        TRY_BTN_X, TRY_BTN_Y - TRY_BTN_H, 0,
        TRY_BTN_X + TRY_BTN_W, TRY_BTN_Y, 0,
        TRY_BTN_X, TRY_BTN_Y, 0
    };
    GLuint bvao, bvbo;
    glGenVertexArrays(1, &bvao); glGenBuffers(1, &bvbo);
    glBindVertexArray(bvao); glBindBuffer(GL_ARRAY_BUFFER, bvbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(bg), bg, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glUseProgram(g_highlight_program);
    glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"), 1, GL_FALSE, id.m);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);
    glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                0.85f, 0.2f, 0.2f, hover ? 0.9f : 0.7f);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0); glDeleteBuffers(1, &bvbo); glDeleteVertexArrays(1, &bvao);

    GLuint tvao, tvbo;
    glGenVertexArrays(1, &tvao); glGenBuffers(1, &tvbo);
    glBindVertexArray(tvao); glBindBuffer(GL_ARRAY_BUFFER, tvbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(text_verts.size()*sizeof(float)),
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

    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 1.0f, 0.3f, 0.3f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, label_count);

    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 1, 1, 1, 1);
    glDrawArrays(GL_TRIANGLES, label_count, total_count - label_count);

    glBindVertexArray(0); glDeleteBuffers(1, &tvbo); glDeleteVertexArrays(1, &tvao);

    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}

// ===========================================================================
// Challenge summary table
// ===========================================================================
void renderer_draw_challenge_summary(const std::string& challenge_name,
                                     const std::vector<SummaryEntry>& entries,
                                     int width, int height) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glViewport(0, 0, width, height);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    Mat4 id = mat4_identity();

    {
        float bg[] = {-0.85f,-0.85f,0, 0.85f,-0.85f,0, 0.85f,0.85f,0,
                      -0.85f,-0.85f,0, 0.85f,0.85f,0, -0.85f,0.85f,0};
        GLuint bvao, bvbo;
        glGenVertexArrays(1, &bvao); glGenBuffers(1, &bvbo);
        glBindVertexArray(bvao); glBindBuffer(GL_ARRAY_BUFFER, bvbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(bg), bg, GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);

        glUseProgram(g_highlight_program);
        glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"), 1, GL_FALSE, id.m);
        glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
        glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"), 0.05f, 0.07f, 0.1f, 0.9f);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0); glDeleteBuffers(1, &bvbo); glDeleteVertexArrays(1, &bvao);
    }

    std::vector<float> text_verts;
    int title_count, subtitle_end, table_start;

    float tcw = 0.045f, tch = 0.065f;
    std::string title = "Challenge Complete!";
    float tw = title.size() * tcw * 0.7f;
    add_screen_string(text_verts, -tw*0.5f, 0.75f, tcw, tch, title);
    title_count = static_cast<int>(text_verts.size() / 5);

    float scw = 0.022f, sch = 0.032f;
    float sw = challenge_name.size() * scw * 0.7f;
    add_screen_string(text_verts, -sw*0.5f, 0.65f, scw, sch, challenge_name);
    subtitle_end = static_cast<int>(text_verts.size() / 5);

    float hcw = 0.020f, hch = 0.028f;
    std::string header = "Puzzle    Your Solution";
    add_screen_string(text_verts, -0.55f, 0.52f, hcw, hch, header);
    table_start = static_cast<int>(text_verts.size() / 5);

    float rcw = 0.018f, rch = 0.026f;
    float row_y = 0.45f;
    float row_h = 0.045f;
    int max_rows = 14;
    int n = std::min(static_cast<int>(entries.size()), max_rows);

    std::vector<int> row_ends;
    for (int i = 0; i < n; i++) {
        const auto& e = entries[i];
        char num[8]; std::snprintf(num, sizeof(num), "%2d.", i + 1);
        std::string row_text = num;
        row_text += "  ";
        for (size_t j = 0; j < e.moves.size(); j++) {
            if (j > 0) row_text += " ";
            row_text += e.moves[j];
        }
        if (e.moves.empty()) row_text += "(no moves)";
        add_screen_string(text_verts, -0.55f, row_y - i * row_h, rcw, rch, row_text);
        row_ends.push_back(static_cast<int>(text_verts.size() / 5));
    }

    float fcw = 0.016f, fch = 0.022f;
    std::string footer = "Click anywhere to return to menu";
    float fw = footer.size() * fcw * 0.7f;
    add_screen_string(text_verts, -fw*0.5f, -0.78f, fcw, fch, footer);
    int footer_end = static_cast<int>(text_verts.size() / 5);

    GLuint tvao, tvbo;
    glGenVertexArrays(1, &tvao); glGenBuffers(1, &tvbo);
    glBindVertexArray(tvao); glBindBuffer(GL_ARRAY_BUFFER, tvbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(text_verts.size()*sizeof(float)),
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

    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 1.0f, 0.9f, 0.4f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, title_count);
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 0.85f, 0.85f, 0.85f, 1.0f);
    glDrawArrays(GL_TRIANGLES, title_count, subtitle_end - title_count);
    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 0.5f, 0.9f, 0.95f, 1.0f);
    glDrawArrays(GL_TRIANGLES, subtitle_end, table_start - subtitle_end);

    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 0.95f, 0.95f, 0.95f, 1.0f);
    int prev = table_start;
    for (int e : row_ends) {
        glDrawArrays(GL_TRIANGLES, prev, e - prev);
        prev = e;
    }

    glUniform4f(glGetUniformLocation(g_text_program, "uColor"), 0.6f, 0.6f, 0.6f, 0.8f);
    glDrawArrays(GL_TRIANGLES, prev, footer_end - prev);

    glBindVertexArray(0);
    glDeleteBuffers(1, &tvbo); glDeleteVertexArrays(1, &tvao);

    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}
