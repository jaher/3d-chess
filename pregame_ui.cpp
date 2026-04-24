#include "pregame_ui.h"

#include "linalg.h"
#include "render_internal.h"
#include "time_control.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// ===========================================================================
// Pre-game setup screen (side toggle + Stockfish ELO slider)
// ===========================================================================
// NDC layout:
//
//   y = +0.55   "Game Setup"            (title)
//   y = +0.25   [ White/Black moves first ] (toggle button)
//   y = -0.05   "Stockfish strength — NNNN"
//   y = -0.25   gradient slider bar (width 1.20, height 0.06)
//                                      + handle below it
//   y = -0.60   [     Start      ]
//   top-left    [ Back ]
//
// The slider's horizontal span MUST match APP_SLIDER_NDC_LEFT/RIGHT in
// app_state.cpp so the pixel-to-ELO math in slider_px_to_elo lines up
// with what's rendered here.
namespace {

constexpr float PG_TOGGLE_W = 0.60f;
constexpr float PG_TOGGLE_H = 0.10f;
constexpr float PG_TOGGLE_X = -PG_TOGGLE_W * 0.5f;
constexpr float PG_TOGGLE_Y =  0.25f;

// Time-control dropdown. Sits between the toggle button and the ELO
// label. When collapsed, only the head is visible; when open, a list
// of TC_COUNT rows drops down below the head and overlaps the ELO
// slider (clicks on it are suppressed while the dropdown is open).
constexpr float PG_TC_HEAD_W =  0.60f;
constexpr float PG_TC_HEAD_H =  0.08f;
constexpr float PG_TC_HEAD_X = -PG_TC_HEAD_W * 0.5f;
constexpr float PG_TC_HEAD_Y =  0.03f;
constexpr float PG_TC_ROW_H  =  0.07f;

constexpr float PG_SLIDER_X_LEFT  = -0.60f;  // mirrors APP_SLIDER_NDC_LEFT
constexpr float PG_SLIDER_X_RIGHT = +0.60f;  // mirrors APP_SLIDER_NDC_RIGHT
constexpr float PG_SLIDER_Y       = -0.30f;
constexpr float PG_SLIDER_H       =  0.07f;
constexpr float PG_SLIDER_STROKE  =  0.006f;

constexpr float PG_START_W = 0.40f;
constexpr float PG_START_H = 0.12f;
constexpr float PG_START_X = -PG_START_W * 0.5f;
constexpr float PG_START_Y = -0.63f;

}  // namespace

int pregame_hit_test(double mx, double my, int width, int height,
                     bool dropdown_open, int* out_tc_index) {
    if (out_tc_index) *out_tc_index = -1;
    float ndc_x = 2.0f * static_cast<float>(mx) / width - 1.0f;
    float ndc_y = 1.0f - 2.0f * static_cast<float>(my) / height;

    // When the dropdown is open it is modal: rows are checked first,
    // then the head (for a click-to-collapse), and finally we return
    // 0 so the caller can treat any other click as "click outside →
    // collapse without changing selection". Crucially this means
    // Start / Toggle / Slider / Back are NOT hit-testable while the
    // dropdown is open.
    if (dropdown_open) {
        float row_y_top = PG_TC_HEAD_Y - PG_TC_HEAD_H;
        for (int i = 0; i < TC_COUNT; ++i) {
            float top = row_y_top - static_cast<float>(i) * PG_TC_ROW_H;
            float bot = top - PG_TC_ROW_H;
            if (ndc_x >= PG_TC_HEAD_X && ndc_x <= PG_TC_HEAD_X + PG_TC_HEAD_W &&
                ndc_y <= top && ndc_y >= bot) {
                if (out_tc_index) *out_tc_index = i;
                return 6;
            }
        }
        if (ndc_x >= PG_TC_HEAD_X && ndc_x <= PG_TC_HEAD_X + PG_TC_HEAD_W &&
            ndc_y >= PG_TC_HEAD_Y - PG_TC_HEAD_H && ndc_y <= PG_TC_HEAD_Y)
            return 5;
        return 0;
    }

    // Back button (same rect as challenge-select back)
    constexpr float BACK_X = -0.95f, BACK_Y = 0.93f;
    constexpr float BACK_W =  0.20f, BACK_H = 0.07f;
    if (ndc_x >= BACK_X && ndc_x <= BACK_X + BACK_W &&
        ndc_y >= BACK_Y - BACK_H && ndc_y <= BACK_Y)
        return 2;

    if (ndc_x >= PG_START_X && ndc_x <= PG_START_X + PG_START_W &&
        ndc_y >= PG_START_Y - PG_START_H && ndc_y <= PG_START_Y)
        return 1;

    if (ndc_x >= PG_TOGGLE_X && ndc_x <= PG_TOGGLE_X + PG_TOGGLE_W &&
        ndc_y >= PG_TOGGLE_Y - PG_TOGGLE_H && ndc_y <= PG_TOGGLE_Y)
        return 3;

    if (ndc_x >= PG_TC_HEAD_X && ndc_x <= PG_TC_HEAD_X + PG_TC_HEAD_W &&
        ndc_y >= PG_TC_HEAD_Y - PG_TC_HEAD_H && ndc_y <= PG_TC_HEAD_Y)
        return 5;

    // Slider — padded by 0.02 NDC vertically and 0.03 horizontally so
    // clicks near the edges still register. Visible bar spans y
    // from PG_SLIDER_Y to PG_SLIDER_Y - PG_SLIDER_H.
    float slider_hit_top    = PG_SLIDER_Y + 0.02f;
    float slider_hit_bottom = PG_SLIDER_Y - PG_SLIDER_H - 0.02f;
    if (ndc_x >= PG_SLIDER_X_LEFT - 0.03f && ndc_x <= PG_SLIDER_X_RIGHT + 0.03f &&
        ndc_y <= slider_hit_top && ndc_y >= slider_hit_bottom)
        return 4;

    return 0;
}

void renderer_draw_pregame(bool human_plays_white,
                           int elo, int elo_min, int elo_max,
                           TimeControl time_control,
                           bool dropdown_open,
                           int tc_hover,
                           int width, int height, int hover) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glViewport(0, 0, width, height);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    Mat4 id = mat4_identity();

    // ----- Flat-color geometry (buttons + slider + handle) -----
    std::vector<float> bg_verts;
    struct Region { int start_vert, count; float r, g, b, a; };
    std::vector<Region> regions;

    auto add_quad = [&](float x, float y, float w, float h,
                        float r, float g, float b, float a) {
        int start = static_cast<int>(bg_verts.size() / 3);
        bg_verts.insert(bg_verts.end(),
            {x,   y-h, 0,  x+w, y-h, 0,  x+w, y, 0,
             x,   y-h, 0,  x+w, y, 0,    x,   y, 0});
        regions.push_back({start, 6, r, g, b, a});
    };

    add_quad(-0.95f, 0.93f, 0.20f, 0.07f,
             0.25f, 0.35f, 0.55f, hover == 2 ? 0.55f : 0.30f);

    if (human_plays_white) {
        add_quad(PG_TOGGLE_X, PG_TOGGLE_Y, PG_TOGGLE_W, PG_TOGGLE_H,
                 0.85f, 0.82f, 0.75f, hover == 3 ? 0.75f : 0.55f);
    } else {
        add_quad(PG_TOGGLE_X, PG_TOGGLE_Y, PG_TOGGLE_W, PG_TOGGLE_H,
                 0.12f, 0.12f, 0.14f, hover == 3 ? 0.95f : 0.78f);
    }

    add_quad(PG_START_X, PG_START_Y, PG_START_W, PG_START_H,
             0.20f, 0.60f, 0.30f, hover == 1 ? 0.75f : 0.55f);

    // Dropdown head in collapsed state. When expanded, the list
    // overlay further down draws on top to give a continuous panel.
    {
        float r = 0.15f, g = 0.18f, b = 0.24f;
        float a = (tc_hover == -2 || dropdown_open) ? 0.95f : 0.85f;
        add_quad(PG_TC_HEAD_X, PG_TC_HEAD_Y,
                 PG_TC_HEAD_W, PG_TC_HEAD_H, r, g, b, a);
    }

    GLuint bvao, bvbo;
    glGenVertexArrays(1, &bvao); glGenBuffers(1, &bvbo);
    glBindVertexArray(bvao);
    glBindBuffer(GL_ARRAY_BUFFER, bvbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(bg_verts.size() * sizeof(float)),
                 bg_verts.data(), GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glUseProgram(g_highlight_program);
    glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"),
                       1, GL_FALSE, id.m);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
    glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);
    glUniform1i(glGetUniformLocation(g_highlight_program, "uUseGradient"), 0);
    for (const auto& r : regions) {
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                    r.r, r.g, r.b, r.a);
        glDrawArrays(GL_TRIANGLES, r.start_vert, r.count);
    }
    glBindVertexArray(0);
    glDeleteBuffers(1, &bvbo); glDeleteVertexArrays(1, &bvao);

    // ----- Pill-shaped progress slider -----
    //
    // Three layered draws form the capsule: outer-pill stroke, inner
    // dark fill, and the green→red gradient that follows the capsule
    // boundary up to fill_x. See the detailed comment below
    // emit_pill_slices for why caps get their own slice count.
    {
        auto emit_pill_slices = [](std::vector<float>& out,
                                   float x_from, float x_to,
                                   float y_mid,
                                   float cap_l_cx, float cap_r_cx,
                                   float R, int cap_slices) {
            if (x_to <= x_from) return;
            auto half_h = [&](float x) -> float {
                if (x <= cap_l_cx) {
                    float dx = cap_l_cx - x;
                    if (dx >= R) return 0;
                    return std::sqrt(R*R - dx*dx);
                }
                if (x >= cap_r_cx) {
                    float dx = x - cap_r_cx;
                    if (dx >= R) return 0;
                    return std::sqrt(R*R - dx*dx);
                }
                return R;
            };
            auto emit_segment = [&](float xa, float xb, int n) {
                if (xb <= xa || n < 1) return;
                for (int i = 0; i < n; i++) {
                    float x0 = xa + static_cast<float>(i)     / n * (xb - xa);
                    float x1 = xa + static_cast<float>(i + 1) / n * (xb - xa);
                    float h0 = half_h(x0);
                    float h1 = half_h(x1);
                    out.insert(out.end(),
                        {x0, y_mid - h0, 0,
                         x1, y_mid - h1, 0,
                         x1, y_mid + h1, 0,
                         x0, y_mid - h0, 0,
                         x1, y_mid + h1, 0,
                         x0, y_mid + h0, 0});
                }
            };

            // Partition [x_from, x_to] into left cap, flat middle,
            // right cap; each gets its own slice count so partial
            // caps stay smooth.
            const float left_cap_start  = x_from;
            const float left_cap_end    = std::min(x_to, cap_l_cx);
            const float mid_start       = std::max(x_from, cap_l_cx);
            const float mid_end         = std::min(x_to, cap_r_cx);
            const float right_cap_start = std::max(x_from, cap_r_cx);
            const float right_cap_end   = x_to;

            if (left_cap_end > left_cap_start && R > 0.0f) {
                float frac = (left_cap_end - left_cap_start) / R;
                int n = std::max(1, static_cast<int>(std::ceil(cap_slices * frac)));
                emit_segment(left_cap_start, left_cap_end, n);
            }
            if (mid_end > mid_start) {
                emit_segment(mid_start, mid_end, 1);
            }
            if (right_cap_end > right_cap_start && R > 0.0f) {
                float frac = (right_cap_end - right_cap_start) / R;
                int n = std::max(1, static_cast<int>(std::ceil(cap_slices * frac)));
                emit_segment(right_cap_start, right_cap_end, n);
            }
        };

        const float R_in     = PG_SLIDER_H * 0.5f;
        const float y_mid_in = PG_SLIDER_Y - R_in;
        const float cap_l_in = PG_SLIDER_X_LEFT  + R_in;
        const float cap_r_in = PG_SLIDER_X_RIGHT - R_in;

        const float R_out     = R_in + PG_SLIDER_STROKE;
        const float y_mid_out = y_mid_in;
        const float x_left_out  = PG_SLIDER_X_LEFT  - PG_SLIDER_STROKE;
        const float x_right_out = PG_SLIDER_X_RIGHT + PG_SLIDER_STROKE;
        const float cap_l_out = x_left_out  + R_out;
        const float cap_r_out = x_right_out - R_out;

        const int CAP_SLICES = 32;

        std::vector<float> pill_verts;
        int outer_start = 0;
        emit_pill_slices(pill_verts,
            x_left_out, x_right_out,
            y_mid_out, cap_l_out, cap_r_out, R_out, CAP_SLICES);
        int outer_count = static_cast<int>(pill_verts.size() / 3) - outer_start;

        int inner_start = static_cast<int>(pill_verts.size() / 3);
        emit_pill_slices(pill_verts,
            PG_SLIDER_X_LEFT, PG_SLIDER_X_RIGHT,
            y_mid_in, cap_l_in, cap_r_in, R_in, CAP_SLICES);
        int inner_count = static_cast<int>(pill_verts.size() / 3) - inner_start;

        float t = static_cast<float>(elo - elo_min) /
                  static_cast<float>(elo_max - elo_min);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        float fill_x = PG_SLIDER_X_LEFT +
                       t * (PG_SLIDER_X_RIGHT - PG_SLIDER_X_LEFT);

        int fill_start = static_cast<int>(pill_verts.size() / 3);
        if (fill_x > PG_SLIDER_X_LEFT) {
            emit_pill_slices(pill_verts,
                PG_SLIDER_X_LEFT, fill_x,
                y_mid_in, cap_l_in, cap_r_in, R_in, CAP_SLICES);
        }
        int fill_count = static_cast<int>(pill_verts.size() / 3) - fill_start;

        GLuint pvao, pvbo;
        glGenVertexArrays(1, &pvao); glGenBuffers(1, &pvbo);
        glBindVertexArray(pvao);
        glBindBuffer(GL_ARRAY_BUFFER, pvbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(pill_verts.size() * sizeof(float)),
                     pill_verts.data(), GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                              3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);

        glUniform1i(glGetUniformLocation(g_highlight_program, "uUseGradient"), 0);
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                    0.72f, 0.76f, 0.90f, 1.0f);
        glDrawArrays(GL_TRIANGLES, outer_start, outer_count);

        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                    0.06f, 0.06f, 0.08f, 1.0f);
        glDrawArrays(GL_TRIANGLES, inner_start, inner_count);

        if (fill_count > 0) {
            glUniform1i(glGetUniformLocation(g_highlight_program, "uUseGradient"), 1);
            glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                        0.20f, 0.85f, 0.25f, 1.0f);
            glUniform4f(glGetUniformLocation(g_highlight_program, "uColorB"),
                        0.92f, 0.20f, 0.18f, 1.0f);
            glUniform1f(glGetUniformLocation(g_highlight_program, "uGradX0"),
                        PG_SLIDER_X_LEFT);
            glUniform1f(glGetUniformLocation(g_highlight_program, "uGradX1"),
                        PG_SLIDER_X_RIGHT);
            glDrawArrays(GL_TRIANGLES, fill_start, fill_count);
        }

        // Reset gradient mode so later draws this frame see a flat-
        // color highlight program.
        glUniform1i(glGetUniformLocation(g_highlight_program, "uUseGradient"), 0);

        glBindVertexArray(0);
        glDeleteBuffers(1, &pvbo); glDeleteVertexArrays(1, &pvao);
    }

    // ----- Text -----
    std::vector<float> ui_verts;

    float tcw = 0.07f, tch = 0.10f;
    std::string title = "Game Setup";
    float tw = title.size() * tcw * 0.7f;
    add_screen_string(ui_verts, -tw * 0.5f, 0.58f, tcw, tch, title);
    int title_count = static_cast<int>(ui_verts.size() / 5);

    float bcw = 0.028f, bch = 0.042f;
    const char* toggle_text_c =
        human_plays_white ? "White moves first" : "Black moves first";
    std::string toggle_text = toggle_text_c;
    float gw = toggle_text.size() * bcw * 0.7f;
    add_screen_string(ui_verts, -gw * 0.5f,
                      PG_TOGGLE_Y - 0.025f, bcw, bch, toggle_text);
    int toggle_end = static_cast<int>(ui_verts.size() / 5);

    float hcw = 0.022f, hch = 0.033f;
    std::string tc_header = "Time control";
    float hw = tc_header.size() * hcw * 0.7f;
    add_screen_string(ui_verts, -hw * 0.5f,
                      PG_TC_HEAD_Y + 0.055f, hcw, hch, tc_header);
    int tc_header_end = static_cast<int>(ui_verts.size() / 5);

    const TimeControlSpec& cur_spec = TIME_CONTROLS[time_control];
    std::string head_label = std::string(cur_spec.short_name) +
                             "  " + cur_spec.display;
    float head_lw = head_label.size() * bcw * 0.7f;
    add_screen_string(ui_verts, -head_lw * 0.5f,
                      PG_TC_HEAD_Y - (PG_TC_HEAD_H - bch) * 0.5f - 0.005f,
                      bcw, bch, head_label);
    int tc_head_text_end = static_cast<int>(ui_verts.size() / 5);

    float ccw = 0.022f, cch = 0.030f;
    std::string chev = dropdown_open ? "^" : "v";
    add_screen_string(ui_verts,
                      PG_TC_HEAD_X + PG_TC_HEAD_W - 0.05f,
                      PG_TC_HEAD_Y - (PG_TC_HEAD_H - cch) * 0.5f - 0.005f,
                      ccw, cch, chev);
    int tc_chev_end = static_cast<int>(ui_verts.size() / 5);

    float scw = 0.025f, sch = 0.038f;
    char elo_buf[64];
    std::snprintf(elo_buf, sizeof(elo_buf), "Stockfish strength  %d", elo);
    std::string elo_label = elo_buf;
    float ew = elo_label.size() * scw * 0.7f;
    add_screen_string(ui_verts, -ew * 0.5f, -0.12f, scw, sch, elo_label);
    int elo_end = static_cast<int>(ui_verts.size() / 5);

    std::string start_text = "Start";
    float stw = start_text.size() * bcw * 0.7f;
    add_screen_string(ui_verts, -stw * 0.5f,
                      PG_START_Y - 0.035f, bcw, bch, start_text);
    int start_end = static_cast<int>(ui_verts.size() / 5);

    float bkw = 0.020f, bkh = 0.030f;
    std::string back_text = "Back";
    add_screen_string(ui_verts, -0.92f, 0.91f, bkw, bkh, back_text);
    int back_end = static_cast<int>(ui_verts.size() / 5);

    GLuint uvao, uvbo;
    glGenVertexArrays(1, &uvao); glGenBuffers(1, &uvbo);
    glBindVertexArray(uvao);
    glBindBuffer(GL_ARRAY_BUFFER, uvbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(ui_verts.size() * sizeof(float)),
                 ui_verts.data(), GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glUseProgram(g_text_program);
    glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"),
                       1, GL_FALSE, id.m);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_font_tex);
    glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);

    glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                1.0f, 0.9f, 0.6f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, title_count);

    if (human_plays_white) {
        glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                    0.10f, 0.10f, 0.10f, 1.0f);
    } else {
        glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                    0.95f, 0.95f, 0.95f, 1.0f);
    }
    glDrawArrays(GL_TRIANGLES, title_count, toggle_end - title_count);

    glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                0.72f, 0.72f, 0.78f, 1.0f);
    glDrawArrays(GL_TRIANGLES, toggle_end, tc_header_end - toggle_end);

    {
        float b = (tc_hover == -2 || dropdown_open) ? 1.0f : 0.92f;
        glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                    b, b, b, 1.0f);
        glDrawArrays(GL_TRIANGLES, tc_header_end,
                     tc_head_text_end - tc_header_end);
    }

    glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                0.80f, 0.80f, 0.85f, 1.0f);
    glDrawArrays(GL_TRIANGLES, tc_head_text_end,
                 tc_chev_end - tc_head_text_end);

    glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                0.85f, 0.85f, 0.85f, 1.0f);
    glDrawArrays(GL_TRIANGLES, tc_chev_end, elo_end - tc_chev_end);

    {
        float b = hover == 1 ? 1.0f : 0.92f;
        glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                    b, b, b, 1.0f);
        glDrawArrays(GL_TRIANGLES, elo_end, start_end - elo_end);
    }

    {
        float b = hover == 2 ? 1.0f : 0.85f;
        glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                    b, b, b, 1.0f);
        glDrawArrays(GL_TRIANGLES, start_end, back_end - start_end);
    }

    glBindVertexArray(0);
    glDeleteBuffers(1, &uvbo); glDeleteVertexArrays(1, &uvao);

    // Expanded dropdown list (drawn last so it overlays everything).
    if (dropdown_open) {
        const float list_top    = PG_TC_HEAD_Y - PG_TC_HEAD_H;
        const float list_bottom = list_top - PG_TC_ROW_H * TC_COUNT;
        const float list_x0     = PG_TC_HEAD_X;
        const float list_x1     = PG_TC_HEAD_X + PG_TC_HEAD_W;

        glUseProgram(g_highlight_program);
        glUniformMatrix4fv(glGetUniformLocation(g_highlight_program, "uMVP"),
                           1, GL_FALSE, id.m);
        glUniform1f(glGetUniformLocation(g_highlight_program, "uInnerRadius"), 0);
        glUniform1f(glGetUniformLocation(g_highlight_program, "uOuterRadius"), 0);
        glUniform1i(glGetUniformLocation(g_highlight_program, "uUseGradient"), 0);
        glUniform1i(glGetUniformLocation(g_highlight_program, "uUseVertexColor"), 0);

        auto push_rect = [](std::vector<float>& v,
                            float x0, float y0, float x1, float y1) {
            v.insert(v.end(),
                {x0, y0, 0,  x1, y0, 0,  x1, y1, 0,
                 x0, y0, 0,  x1, y1, 0,  x0, y1, 0});
        };

        // 1. Outline rectangle (slightly larger behind the list bg).
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                    0.55f, 0.60f, 0.72f, 0.95f);
        {
            std::vector<float> ov;
            push_rect(ov,
                      list_x0 - 0.006f, list_bottom - 0.010f,
                      list_x1 + 0.006f, list_top    + 0.003f);
            GLuint ovao, ovbo;
            glGenVertexArrays(1, &ovao); glGenBuffers(1, &ovbo);
            glBindVertexArray(ovao); glBindBuffer(GL_ARRAY_BUFFER, ovbo);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(ov.size() * sizeof(float)),
                         ov.data(), GL_STREAM_DRAW);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                                  3 * sizeof(float), (void*)0);
            glEnableVertexAttribArray(0);
            glDrawArrays(GL_TRIANGLES, 0,
                         static_cast<GLsizei>(ov.size() / 3));
            glBindVertexArray(0);
            glDeleteBuffers(1, &ovbo); glDeleteVertexArrays(1, &ovao);
        }

        // 2. List background.
        glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                    0.10f, 0.12f, 0.16f, 0.98f);
        {
            std::vector<float> bv;
            push_rect(bv, list_x0, list_bottom, list_x1, list_top);
            GLuint bvao2, bvbo2;
            glGenVertexArrays(1, &bvao2); glGenBuffers(1, &bvbo2);
            glBindVertexArray(bvao2); glBindBuffer(GL_ARRAY_BUFFER, bvbo2);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(bv.size() * sizeof(float)),
                         bv.data(), GL_STREAM_DRAW);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                                  3 * sizeof(float), (void*)0);
            glEnableVertexAttribArray(0);
            glDrawArrays(GL_TRIANGLES, 0,
                         static_cast<GLsizei>(bv.size() / 3));
            glBindVertexArray(0);
            glDeleteBuffers(1, &bvbo2); glDeleteVertexArrays(1, &bvao2);
        }

        // 3. Row highlights: gold for the selected row, slate for
        //    the hovered row.
        for (int i = 0; i < TC_COUNT; ++i) {
            float top = list_top - static_cast<float>(i) * PG_TC_ROW_H;
            float bot = top - PG_TC_ROW_H;
            bool selected = (i == (int)time_control);
            bool hovered  = (i == tc_hover);
            if (!selected && !hovered) continue;

            float r, g, b, a;
            if (selected && hovered) {
                r = 0.90f; g = 0.72f; b = 0.22f; a = 0.95f;
            } else if (selected) {
                r = 0.78f; g = 0.60f; b = 0.15f; a = 0.92f;
            } else {
                r = 0.22f; g = 0.28f; b = 0.38f; a = 0.75f;
            }
            glUniform4f(glGetUniformLocation(g_highlight_program, "uColor"),
                        r, g, b, a);
            std::vector<float> rv;
            push_rect(rv, list_x0, bot, list_x1, top);
            GLuint rvao, rvbo;
            glGenVertexArrays(1, &rvao); glGenBuffers(1, &rvbo);
            glBindVertexArray(rvao); glBindBuffer(GL_ARRAY_BUFFER, rvbo);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(rv.size() * sizeof(float)),
                         rv.data(), GL_STREAM_DRAW);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                                  3 * sizeof(float), (void*)0);
            glEnableVertexAttribArray(0);
            glDrawArrays(GL_TRIANGLES, 0,
                         static_cast<GLsizei>(rv.size() / 3));
            glBindVertexArray(0);
            glDeleteBuffers(1, &rvbo); glDeleteVertexArrays(1, &rvao);
        }

        // 4. Row labels.
        std::vector<float> lv;
        float rcw = 0.028f, rch = 0.042f;
        for (int i = 0; i < TC_COUNT; ++i) {
            float top = list_top - static_cast<float>(i) * PG_TC_ROW_H;
            const TimeControlSpec& s = TIME_CONTROLS[i];
            std::string label = std::string(s.short_name) + "  " + s.display;
            float lw = label.size() * rcw * 0.7f;
            add_screen_string(lv, -lw * 0.5f,
                              top - (PG_TC_ROW_H - rch) * 0.5f - 0.005f,
                              rcw, rch, label);
        }
        int lv_count = static_cast<int>(lv.size() / 5);
        if (lv_count > 0) {
            GLuint lvao, lvbo;
            glGenVertexArrays(1, &lvao); glGenBuffers(1, &lvbo);
            glBindVertexArray(lvao); glBindBuffer(GL_ARRAY_BUFFER, lvbo);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(lv.size() * sizeof(float)),
                         lv.data(), GL_STREAM_DRAW);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                                  5 * sizeof(float), (void*)0);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                                  5 * sizeof(float),
                                  (void*)(3 * sizeof(float)));
            glEnableVertexAttribArray(1);

            glUseProgram(g_text_program);
            glUniformMatrix4fv(glGetUniformLocation(g_text_program, "uMVP"),
                               1, GL_FALSE, id.m);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_font_tex);
            glUniform1i(glGetUniformLocation(g_text_program, "uFontTex"), 0);
            glUniform4f(glGetUniformLocation(g_text_program, "uColor"),
                        0.97f, 0.97f, 0.94f, 1.0f);
            glDrawArrays(GL_TRIANGLES, 0, lv_count);
            glBindVertexArray(0);
            glDeleteBuffers(1, &lvbo); glDeleteVertexArrays(1, &lvao);
        }
    }

    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}
