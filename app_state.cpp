#include "app_state.h"

#include "ai_player.h"
#include "audio.h"
#include "chess_rules.h"
#include "cloth_flag.h"
#include "linalg.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// ===========================================================================
// Small helpers
// ===========================================================================
namespace {

int64_t now_us(const AppState& a) {
    return a.platform ? a.platform->now_us() : 0;
}

void set_status(const AppState& a, const char* text) {
    if (a.platform && a.platform->set_status) a.platform->set_status(text);
}

void queue_redraw(const AppState& a) {
    if (a.platform && a.platform->queue_redraw) a.platform->queue_redraw();
}

std::string current_fen(const GameState& gs, bool white_turn) {
    BoardSquare board[8][8];
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            board[r][c] = {-1, false};
    for (const auto& p : gs.pieces)
        if (p.alive)
            board[p.row][p.col] = {p.type, p.is_white};
    return board_to_fen(board, white_turn,
        gs.castling.white_king_moved,  gs.castling.black_king_moved,
        gs.castling.white_rook_a_moved, gs.castling.white_rook_h_moved,
        gs.castling.black_rook_a_moved, gs.castling.black_rook_h_moved);
}

int env_int(const char* name, int fallback) {
    if (const char* v = std::getenv(name)) {
        int n = std::atoi(v);
        if (n > 0) return n;
    }
    return fallback;
}

} // namespace

// ===========================================================================
// Status text formatting
// ===========================================================================
static void refresh_play_status(AppState& a) {
    const GameState& gs = a.game;
    if (gs.ai_thinking) {
        set_status(a, "3D Chess — AI thinking...");
        return;
    }
    if (gs.game_over) {
        std::string s = "3D Chess — " + gs.game_result;
        set_status(a, s.c_str());
        return;
    }
    float score = evaluate_position(gs);
    char buf[160];
    const char* turn = gs.white_turn ? "White" : "Black";
    bool check = is_in_check(const_cast<GameState&>(gs), gs.white_turn);
    if (std::abs(score) < 0.1f) {
        std::snprintf(buf, sizeof(buf), "3D Chess — %s to move%s — Even",
                      turn, check ? " (CHECK)" : "");
    } else if (score > 0) {
        std::snprintf(buf, sizeof(buf), "3D Chess — %s to move%s — White +%.1f",
                      turn, check ? " (CHECK)" : "", score);
    } else {
        std::snprintf(buf, sizeof(buf), "3D Chess — %s to move%s — Black +%.1f",
                      turn, check ? " (CHECK)" : "", -score);
    }
    set_status(a, buf);
}

static void refresh_analysis_status(AppState& a) {
    const GameState& gs = a.game;
    int total = static_cast<int>(gs.snapshots.size()) - 1;
    char buf[160];
    if (gs.analysis_index == 0) {
        std::snprintf(buf, sizeof(buf),
                      "ANALYSIS — Starting position [0/%d] — Left/Right to navigate, Esc to exit",
                      total);
    } else {
        const auto& snap = gs.snapshots[gs.analysis_index];
        int move_num = (gs.analysis_index + 1) / 2;
        const char* side = (gs.analysis_index % 2 == 1) ? "White" : "Black";
        std::snprintf(buf, sizeof(buf),
                      "ANALYSIS — Move %d. %s %s [%d/%d] — Left/Right, Esc to exit",
                      move_num, side, snap.last_move.c_str(),
                      gs.analysis_index, total);
    }
    set_status(a, buf);
}

void app_refresh_status(AppState& a) {
    if (a.game.analysis_mode) refresh_analysis_status(a);
    else                       refresh_play_status(a);
}

// ===========================================================================
// Async AI / eval dispatch (through the platform)
// ===========================================================================
static void trigger_ai(AppState& a) {
    a.game.ai_thinking = true;
    set_status(a, "3D Chess — AI thinking...");
    queue_redraw(a);
    // Pass the actual current side-to-move; Stockfish plays whichever
    // side the human isn't.
    std::string fen = current_fen(a.game, a.game.white_turn);
    // Move time: an explicit env var override wins; otherwise use
    // ~1/30 of the AI's own remaining clock, clamped so bullet is
    // responsive (≤2s) and classical doesn't drag (≤3s). In
    // Unlimited mode we fall back to the legacy 800ms default.
    int movetime = env_int("CHESS_AI_MOVETIME_MS", 0);
    if (movetime <= 0) {
        if (!a.clock_enabled) {
            movetime = 800;
        } else {
            int64_t ai_ms = a.human_plays_white ? a.black_ms_left
                                                : a.white_ms_left;
            movetime = static_cast<int>(ai_ms / 30);
            if (movetime < 200)  movetime = 200;
            if (movetime > 3000) movetime = 3000;
        }
    }
    if (a.platform && a.platform->trigger_ai_move)
        a.platform->trigger_ai_move(fen.c_str(), movetime);
}

static void trigger_eval(AppState& a, int score_index) {
    std::string fen = current_fen(a.game, a.game.white_turn);
    int movetime = env_int("CHESS_EVAL_MOVETIME_MS", 150);
    if (a.platform && a.platform->trigger_eval)
        a.platform->trigger_eval(fen.c_str(), movetime, score_index);
}

// Play the appropriate SFX for a move that's already been applied to
// ``gs``. The caller passes the capture flag it read from the
// position BEFORE calling ``execute_move`` — after the move, that
// info is gone from the grid. Check is computed from the post-move
// state. Priority: check > capture > plain move (castling reuses
// the plain-move sound).
static void play_move_sfx(const GameState& gs_after, bool was_capture) {
    bool opponent_white = gs_after.white_turn;
    bool in_check = is_in_check(gs_after, opponent_white);
    if (in_check)         audio_play(SoundEffect::Check);
    else if (was_capture) audio_play(SoundEffect::Capture);
    else                  audio_play(SoundEffect::Move);
}

// ===========================================================================
// Screen-to-board picking
// ===========================================================================
// Build a world-space ray from a mouse pixel position. Shared
// between the flat-plane click test and the ray-vs-mesh piece pick.
static bool screen_ray(const AppState& a, double mx, double my,
                       int width, int height,
                       float ro[3], float rd[3]) {
    float deg2rad = static_cast<float>(M_PI) / 180.0f;
    float aspect = static_cast<float>(width) / static_cast<float>(height);
    Mat4 view = mat4_multiply(
        mat4_translate(0, 0, -a.zoom),
        mat4_multiply(mat4_rotate_x(a.rot_x * deg2rad),
                      mat4_multiply(mat4_rotate_y(a.rot_y * deg2rad),
                                    mat4_translate(0, -BOARD_Y, 0))));
    Mat4 proj = mat4_perspective(45.0f * deg2rad, aspect, 0.1f, 100.0f);
    Mat4 inv_vp = mat4_inverse(mat4_multiply(proj, view));

    float ndc_x = 2.0f * static_cast<float>(mx) / width - 1.0f;
    float ndc_y = 1.0f - 2.0f * static_cast<float>(my) / height;

    Vec4 nw = mat4_mul_vec4(inv_vp, {ndc_x, ndc_y, -1, 1});
    Vec4 fw = mat4_mul_vec4(inv_vp, {ndc_x, ndc_y,  1, 1});
    if (std::abs(nw.w) < 1e-10f || std::abs(fw.w) < 1e-10f) return false;

    ro[0] = nw.x / nw.w; ro[1] = nw.y / nw.w; ro[2] = nw.z / nw.w;
    float fx = fw.x / fw.w - ro[0];
    float fy = fw.y / fw.w - ro[1];
    float fz = fw.z / fw.w - ro[2];
    float len = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (len < 1e-10f) return false;
    rd[0] = fx / len; rd[1] = fy / len; rd[2] = fz / len;
    return true;
}

static bool screen_to_board(const AppState& a, double mx, double my,
                            int width, int height, int& out_col, int& out_row) {
    float ro[3], rd[3];
    if (!screen_ray(a, mx, my, width, height, ro, rd)) return false;
    if (std::abs(rd[1]) < 1e-10f) return false;

    float t = (BOARD_Y - ro[1]) / rd[1];
    if (t < 0) return false;

    out_col = static_cast<int>(std::floor((ro[0] + t * rd[0]) / SQ + 4.0f));
    out_row = static_cast<int>(std::floor((ro[2] + t * rd[2]) / SQ + 4.0f));
    return in_bounds(out_col, out_row);
}

// ---------------------------------------------------------------------------
// Ray-vs-mesh piece picking
// ---------------------------------------------------------------------------
// Möller-Trumbore ray-triangle intersection. The direction does
// NOT need to be unit-length: the resulting t is in the same units
// as `rd`, so if the caller passes a direction transformed by an
// inverse model matrix (unnormalized), the t it gets back is
// directly comparable to t values from other pieces under the same
// ray (because they're all in world units).
static bool ray_triangle_hit(const float ro[3], const float rd[3],
                             const Vertex& v0, const Vertex& v1,
                             const Vertex& v2, float& out_t) {
    const float e1x = v1.x - v0.x, e1y = v1.y - v0.y, e1z = v1.z - v0.z;
    const float e2x = v2.x - v0.x, e2y = v2.y - v0.y, e2z = v2.z - v0.z;
    const float hx = rd[1] * e2z - rd[2] * e2y;
    const float hy = rd[2] * e2x - rd[0] * e2z;
    const float hz = rd[0] * e2y - rd[1] * e2x;
    const float a = e1x * hx + e1y * hy + e1z * hz;
    if (std::fabs(a) < 1e-8f) return false;
    const float f = 1.0f / a;
    const float sx = ro[0] - v0.x, sy = ro[1] - v0.y, sz = ro[2] - v0.z;
    const float u = f * (sx * hx + sy * hy + sz * hz);
    if (u < 0.0f || u > 1.0f) return false;
    const float qx = sy * e1z - sz * e1y;
    const float qy = sz * e1x - sx * e1z;
    const float qz = sx * e1y - sy * e1x;
    const float v = f * (rd[0] * qx + rd[1] * qy + rd[2] * qz);
    if (v < 0.0f || u + v > 1.0f) return false;
    const float t = f * (e2x * qx + e2y * qy + e2z * qz);
    if (t < 1e-4f) return false;
    out_t = t;
    return true;
}

// Slab-method ray-AABB test. Used as an early reject before the
// per-triangle loop. If the ray misses the piece's bounding box in
// object space we save ~80k triangle tests.
static bool ray_aabb_hit(const float ro[3], const float rd[3],
                         const Vertex& bmin, const Vertex& bmax) {
    float tmin = -1e30f, tmax = 1e30f;
    const float rox[3] = { ro[0], ro[1], ro[2] };
    const float rdx[3] = { rd[0], rd[1], rd[2] };
    const float bn[3]  = { bmin.x, bmin.y, bmin.z };
    const float bx[3]  = { bmax.x, bmax.y, bmax.z };
    for (int i = 0; i < 3; i++) {
        if (std::fabs(rdx[i]) < 1e-8f) {
            if (rox[i] < bn[i] || rox[i] > bx[i]) return false;
        } else {
            const float inv = 1.0f / rdx[i];
            float t1 = (bn[i] - rox[i]) * inv;
            float t2 = (bx[i] - rox[i]) * inv;
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return false;
        }
    }
    return tmax >= 0.0f;
}

// Ray-vs-mesh piece picking. Iterates every alive piece, builds
// its world-to-raw-STL transform, intersects the ray against the
// piece's STL triangles, and returns the index (into gs.pieces) of
// the closest positive-t hit. Returns -1 on miss or when no STL
// models are available.
//
// Coordinate-space note: the GPU vertex buffer for each piece is
// `(v_raw - bbox_center) * (2 / bbox_max_extent)` — a normalised
// unit-sphere fit. The render-time model matrix in board_renderer
// is `translate(wx, BOARD_Y+s, wz) * scale(s) * orient`, which
// premultiplies that normalised space. For picking we want to
// intersect against the raw STL triangles directly (so we don't
// have to rebuild a normalised vertex list), so we fold the
// normalisation into the transform:
//
//   M = T(wx, BOARD_Y+s, wz) * S(s) * orient * S(norm) * T(-center)
//
// and then M_inv carries the world ray into raw-STL space. The
// unnormalised inverse direction preserves the world-space metric,
// so the t values are directly comparable across pieces.
static int pick_piece(const AppState& a, double mx, double my,
                      int width, int height) {
    if (!a.loaded_models) return -1;

    float ro[3], rd[3];
    if (!screen_ray(a, mx, my, width, height, ro, rd)) return -1;

    const GameState& gs = a.game;
    const float deg2rad = static_cast<float>(M_PI) / 180.0f;
    const float rot_z_to_y = -90.0f * deg2rad;

    int best_idx = -1;
    float best_t = 1e30f;

    for (size_t i = 0; i < gs.pieces.size(); i++) {
        const BoardPiece& bp = gs.pieces[i];
        if (!bp.alive) continue;

        float wx, wz;
        square_center(bp.col, bp.row, wx, wz);
        const float s = BASE_PIECE_SCALE * piece_scale[bp.type];

        const StlModel& model = a.loaded_models[bp.type];
        const BoundingBox& bb = model.bounding_box();
        if (model.triangles().empty()) continue;
        const float max_ext = bb.max_extent();
        if (max_ext < 1e-6f) continue;
        const float norm_scale = 2.0f / max_ext;
        const Vertex center = bb.center();

        // Build M = T_piece * S_piece * orient * S_norm * T(-center)
        Mat4 orient = mat4_rotate_x(rot_z_to_y);
        if (!bp.is_white)
            orient = mat4_multiply(mat4_rotate_y(static_cast<float>(M_PI)),
                                   orient);
        Mat4 M = mat4_multiply(
            mat4_translate(wx, BOARD_Y + s, wz),
            mat4_multiply(mat4_scale(s, s, s),
                mat4_multiply(orient,
                    mat4_multiply(
                        mat4_scale(norm_scale, norm_scale, norm_scale),
                        mat4_translate(-center.x, -center.y, -center.z)))));
        Mat4 M_inv = mat4_inverse(M);

        // Transform the world ray into raw-STL space. Keep the
        // direction UNNORMALISED so that t from the triangle
        // intersection is still in world units.
        Vec4 ro_obj4 = mat4_mul_vec4(M_inv, {ro[0], ro[1], ro[2], 1.0f});
        Vec4 rd_obj4 = mat4_mul_vec4(M_inv, {rd[0], rd[1], rd[2], 0.0f});
        float ro_obj[3] = { ro_obj4.x, ro_obj4.y, ro_obj4.z };
        float rd_obj[3] = { rd_obj4.x, rd_obj4.y, rd_obj4.z };

        // AABB early-reject against the raw-STL bounding box.
        if (!ray_aabb_hit(ro_obj, rd_obj, bb.min, bb.max)) continue;

        float piece_t = 1e30f;
        bool piece_hit = false;
        const auto& tris = model.triangles();
        for (const auto& tri : tris) {
            float t;
            if (ray_triangle_hit(ro_obj, rd_obj, tri.v0, tri.v1, tri.v2, t)) {
                if (t < piece_t) {
                    piece_t = t;
                    piece_hit = true;
                }
            }
        }
        if (piece_hit && piece_t < best_t) {
            best_t = piece_t;
            best_idx = static_cast<int>(i);
        }
    }

    return best_idx;
}

// ===========================================================================
// Board click handling
// ===========================================================================
static void handle_board_click(AppState& a, double mx, double my,
                               int width, int height) {
    GameState& gs = a.game;
    bool is_challenge = (a.mode == MODE_CHALLENGE);
    bool is_normal_game = (a.mode == MODE_PLAYING);

    if (is_normal_game) {
        if (gs.ai_thinking || gs.ai_animating || gs.analysis_mode ||
            gs.white_turn != a.human_plays_white || gs.game_over)
            return;
    } else if (is_challenge) {
        if (gs.game_over || a.challenge_solved || a.challenge_mistake) return;
        int max_moves = a.current_challenge.max_moves;
        if (max_moves > 0) {
            bool starter_to_move =
                (gs.white_turn == a.current_challenge.starts_white);
            if (starter_to_move && a.challenge_moves_made >= max_moves) return;
        }
    }

    // Ray-vs-mesh pick first: if the cursor ray hits an actual
    // piece model, use that piece's square as the click target so
    // tall pieces that lean forward over a neighbouring square
    // still select correctly. Fall back to the flat-plane test
    // when the ray misses all meshes (empty squares / move
    // destinations).
    int col, row;
    int hit_piece = pick_piece(a, mx, my, width, height);
    if (hit_piece >= 0) {
        col = gs.pieces[hit_piece].col;
        row = gs.pieces[hit_piece].row;
    } else if (!screen_to_board(a, mx, my, width, height, col, row)) {
        gs.selected_col = gs.selected_row = -1;
        gs.valid_moves.clear();
        queue_redraw(a);
        return;
    }

    // Clicking the currently-selected piece again deselects it.
    // Do this before the "is this a valid move?" loop so the
    // click doesn't get interpreted as a no-op move to the same
    // square (which wouldn't execute anyway, but this is the
    // intent-level behaviour the user expects).
    if (gs.selected_col == col && gs.selected_row == row) {
        gs.selected_col = gs.selected_row = -1;
        gs.valid_moves.clear();
        queue_redraw(a);
        return;
    }

    if (gs.selected_col >= 0) {
        for (const auto& [mc, mr] : gs.valid_moves) {
            if (mc == col && mr == row) {
                bool was_starter =
                    (gs.white_turn == a.current_challenge.starts_white);
                // Snapshot the grid BEFORE the move so we can tell
                // whether the destination was occupied (capture) —
                // that info is gone once execute_move runs.
                bool sfx_capture = gs.grid[row][col] >= 0;
                execute_move(gs, gs.selected_col, gs.selected_row, col, row);
                gs.selected_col = gs.selected_row = -1;
                gs.valid_moves.clear();
                play_move_sfx(gs, sfx_capture);
                queue_redraw(a);

                if (is_challenge) {
                    if (was_starter) a.challenge_moves_made++;
                    if (!gs.move_history.empty() && gs.snapshots.size() >= 2) {
                        int pi = a.current_challenge.current_index;
                        if (pi >= 0 &&
                            pi < static_cast<int>(a.challenge_solutions.size())) {
                            const auto& before =
                                gs.snapshots[gs.snapshots.size() - 2];
                            std::string alg = uci_to_algebraic(
                                before, gs.move_history.back());
                            a.challenge_solutions[pi].push_back(alg);
                        }
                    }
                    // Tactic-puzzle success: find_forks / find_pins
                    // require the user to enumerate EVERY legal move
                    // that creates the motif. Each attempt is checked
                    // against the precomputed required-move list,
                    // then the board is reset so they can play the
                    // next candidate from the original position.
                    if (was_starter) {
                        const std::string& ct = a.current_challenge.type;
                        bool is_tactic = (ct == "find_forks" ||
                                           ct == "find_pins");
                        if (is_tactic) {
                            std::string uci = gs.move_history.empty()
                                ? std::string()
                                : gs.move_history.back();
                            const auto& req =
                                a.current_challenge.required_moves;
                            auto& found =
                                a.current_challenge.found_moves;
                            bool is_required = std::find(
                                req.begin(), req.end(), uci) != req.end();
                            bool already_found = std::find(
                                found.begin(), found.end(), uci) != found.end();
                            if (is_required && !already_found) {
                                found.push_back(uci);
                            }

                            // Reset the board to the puzzle's starting
                            // FEN so the user can play the next move.
                            int pi = a.current_challenge.current_index;
                            ParsedFEN p = parse_fen(
                                a.current_challenge.fens[pi]);
                            if (p.valid) apply_fen_to_state(a.game, p);
                            a.challenge_moves_made = 0;

                            if (!req.empty() && found.size() >= req.size()) {
                                a.challenge_solved = true;
                            }

                            char buf[160];
                            const char* noun =
                                (ct == "find_forks") ? "forks" : "pins";
                            std::snprintf(
                                buf, sizeof(buf),
                                "%s — found %d / %d %s",
                                is_required ? "Good!"
                                             : "Not a match — try again",
                                static_cast<int>(found.size()),
                                static_cast<int>(req.size()),
                                noun
                            );
                            set_status(a, buf);
                            queue_redraw(a);
                            return;
                        }
                    }
                    if (gs.game_over) {
                        bool solved = false;
                        if (a.current_challenge.starts_white &&
                            gs.game_result.find("White wins") != std::string::npos)
                            solved = true;
                        if (!a.current_challenge.starts_white &&
                            gs.game_result.find("Black wins") != std::string::npos)
                            solved = true;
                        if (solved) a.challenge_solved = true;
                    }
                    // Mistake: starter used up their move budget without
                    // delivering mate. Kick off the shake + sfx; the
                    // Try-Again button renders after the shake settles.
                    if (was_starter && !a.challenge_solved &&
                        a.current_challenge.max_moves > 0 &&
                        a.challenge_moves_made >= a.current_challenge.max_moves &&
                        !a.challenge_mistake) {
                        a.challenge_mistake = true;
                        a.challenge_mistake_start_us = now_us(a);
                        a.challenge_try_again_hover = false;
                        audio_play(SoundEffect::Mistake);
                    }
                } else {
                    app_refresh_status(a);
                    trigger_eval(
                        a, static_cast<int>(gs.score_history.size()) - 1);
                    if (gs.white_turn != a.human_plays_white && !gs.game_over)
                        trigger_ai(a);
                }
                return;
            }
        }
    }

    int idx = gs.grid[row][col];
    if (idx >= 0 && gs.pieces[idx].is_white == gs.white_turn) {
        gs.selected_col = col;
        gs.selected_row = row;
        gs.valid_moves = generate_legal_moves(gs, col, row);
        gs.anim_start_time = now_us(a);
    } else {
        gs.selected_col = gs.selected_row = -1;
        gs.valid_moves.clear();
    }
    queue_redraw(a);
}

// ===========================================================================
// Mode transitions
// ===========================================================================
void app_enter_menu(AppState& a) {
    // Exit analysis mode if we're in it, so the next game starts
    // clean even if the user clicked Back to Menu mid-analysis.
    if (a.game.analysis_mode) game_exit_analysis(a.game);

    a.mode = MODE_MENU;
    menu_init_physics(a.menu_pieces);
    a.menu_start_time_us  = now_us(a);
    a.menu_last_update_us = a.menu_start_time_us;
    a.withdraw_confirm_open = false;
    a.withdraw_hover = 0;
    a.endgame_menu_hover = false;
    a.continue_playing_hover = false;
    audio_music_play("intro_music.wav");
    set_status(a, "3D Chess");
    queue_redraw(a);
}

void app_enter_pregame(AppState& a) {
    a.mode = MODE_PREGAME;
    // Preserve a.human_plays_white, a.stockfish_elo, a.time_control
    // across reopens. Volatile state (drags, hovers, open dropdowns)
    // must be reset so re-entering the screen doesn't land in a
    // weird half-expanded state.
    a.slider_dragging = false;
    a.pregame_hover = 0;
    a.pregame_tc_open = false;
    a.pregame_tc_hover = -1;
    set_status(a, "3D Chess — Game Setup");
    queue_redraw(a);
}

// ---------------------------------------------------------------------------
// Pregame slider helpers
// ---------------------------------------------------------------------------
// The slider covers both Stockfish operating modes: at elo >= 1320
// the engine uses UCI_LimitStrength + UCI_Elo (documented floor),
// below that it switches to the Skill Level option to reach weaker
// play. See ai_player.cpp::apply_elo for the mapping.
static constexpr int APP_ELO_MIN = 800;
static constexpr int APP_ELO_MAX = 2850;

// Convert a mouse x coordinate in pixel space to an ELO value.
// The slider's horizontal span is defined by PREGAME_SLIDER_X_LEFT /
// PREGAME_SLIDER_WIDTH in board_renderer.cpp; we mirror the NDC
// bounds here so app_state doesn't have to know about GL.
static constexpr float APP_SLIDER_NDC_LEFT  = -0.60f;
static constexpr float APP_SLIDER_NDC_RIGHT = +0.60f;

static int slider_px_to_elo(double mx, int width) {
    float ndc_x = 2.0f * static_cast<float>(mx) / width - 1.0f;
    float t = (ndc_x - APP_SLIDER_NDC_LEFT) /
              (APP_SLIDER_NDC_RIGHT - APP_SLIDER_NDC_LEFT);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return APP_ELO_MIN + static_cast<int>(
        std::round(t * (APP_ELO_MAX - APP_ELO_MIN)));
}

void app_enter_game(AppState& a) {
    a.mode = MODE_PLAYING;
    audio_music_stop();
    game_reset(a.game);
    a.rot_x = 30.0f;
    // Camera points at whichever side the human is playing so their
    // pieces are at the bottom of the screen.
    a.rot_y = a.human_plays_white ? 180.0f : 0.0f;
    a.zoom  = 12.0f;
    if (a.platform && a.platform->set_ai_elo)
        a.platform->set_ai_elo(a.stockfish_elo);
    // Withdraw modal and button-hover state — clean slate per game.
    a.withdraw_confirm_open = false;
    a.withdraw_hover = 0;
    a.endgame_menu_hover = false;
    a.continue_playing_hover = false;
    // Flag is (re)initialised lazily on the first frame we know the
    // window size, either from app_tick or the first renderer_draw
    // dispatch. Mark it uninitialised by zeroing inited_w/h.
    a.flag.inited_w = 0;
    a.flag.inited_h = 0;
    a.flag_last_update_us = 0;
    a.flag_start_us = now_us(a);
    // Clocks — refill both sides with the starting budget for the
    // selected time control. Unlimited leaves base_ms at 0 which
    // disables the clock widget and the tick loop entirely.
    {
        const TimeControlSpec& spec = TIME_CONTROLS[a.time_control];
        a.clock_enabled = (a.time_control != TC_UNLIMITED);
        a.white_ms_left = spec.base_ms;
        a.black_ms_left = spec.base_ms;
        a.clock_last_tick_us = 0;
        a.prev_white_turn = -1;
    }
    app_refresh_status(a);
    queue_redraw(a);
    // If the human plays black, Stockfish makes the first move as white.
    if (!a.human_plays_white) trigger_ai(a);
}

void app_enter_challenge_select(AppState& a) {
    a.mode = MODE_CHALLENGE_SELECT;
    a.challenge_files = list_challenge_files("challenges");
    a.challenge_names.clear();
    for (const auto& f : a.challenge_files) {
        Challenge c = load_challenge(f);
        a.challenge_names.push_back(c.name);
    }
    a.challenge_select_hover = -1;
    set_status(a, "Select Challenge");
    queue_redraw(a);
}

void app_load_challenge_puzzle(AppState& a, int puzzle_index) {
    if (puzzle_index < 0 ||
        puzzle_index >= static_cast<int>(a.current_challenge.fens.size()))
        return;
    // Sync ch.type/max_moves/starts_white to the active puzzle so a
    // multi-page homework file with mixed tactic types behaves
    // correctly on navigation.
    challenge_apply_current(a.current_challenge, puzzle_index);
    a.challenge_moves_made = 0;
    a.challenge_solved = false;
    a.challenge_next_hover = false;
    a.challenge_mistake = false;
    a.challenge_try_again_hover = false;
    a.board_shake_x = 0.0f;
    ParsedFEN parsed = parse_fen(a.current_challenge.fens[puzzle_index]);
    if (parsed.valid) apply_fen_to_state(a.game, parsed);

    // For find_forks / find_pins puzzles, pre-compute every legal
    // move that produces the target motif so we can track progress
    // as the user plays them one at a time.
    const std::string& ct = a.current_challenge.type;
    if (ct == "find_forks" || ct == "find_pins") {
        a.current_challenge.required_moves = find_tactic_moves(
            a.game, a.current_challenge.starts_white, ct
        );
    }
    char buf[160];
    std::snprintf(buf, sizeof(buf), "Challenge: %s [%d/%d]",
                  a.current_challenge.name.c_str(),
                  puzzle_index + 1,
                  static_cast<int>(a.current_challenge.fens.size()));
    set_status(a, buf);
    queue_redraw(a);
}

void app_reset_challenge_puzzle(AppState& a) {
    int idx = a.current_challenge.current_index;
    if (idx >= 0 &&
        idx < static_cast<int>(a.challenge_solutions.size()))
        a.challenge_solutions[idx].clear();
    app_load_challenge_puzzle(a, idx);
}

void app_enter_challenge(AppState& a, int index) {
    if (index < 0 ||
        index >= static_cast<int>(a.challenge_files.size())) return;
    a.current_challenge = load_challenge(a.challenge_files[index]);
    if (a.current_challenge.fens.empty()) return;
    a.mode = MODE_CHALLENGE;
    audio_music_stop();
    a.challenge_solutions.assign(a.current_challenge.fens.size(), {});
    a.challenge_show_summary = false;
    app_load_challenge_puzzle(a, 0);
}

// ===========================================================================
// Input
// ===========================================================================
void app_press(AppState& a, double mx, double my) {
    a.dragging = true;
    a.last_mouse_x = a.press_x = mx;
    a.last_mouse_y = a.press_y = my;

    // Browsers block audio playback until a user gesture. Kick the
    // music track on every "menu-ish" screen press so the intro
    // loop actually starts on web — the call is idempotent, so
    // subsequent presses are no-ops.
    if (a.mode == MODE_MENU ||
        a.mode == MODE_PREGAME ||
        a.mode == MODE_CHALLENGE_SELECT) {
        audio_music_play("intro_music.wav");
    }

    // On the pregame screen, pressing inside the slider hit area begins
    // a drag. We need the canvas size to know where the slider is, but
    // app_press doesn't receive it — use the last known size from
    // app_motion by querying pregame_hit_test with an estimate. In
    // practice the press always immediately precedes a motion, so the
    // drag flag is promptly confirmed in app_motion.
    if (a.mode == MODE_PREGAME) {
        // Defer actual hit-test to app_motion/app_release, which have
        // width/height. Just mark "might be slider drag" via last_mouse.
        a.slider_dragging = false;
    }
}

void app_release(AppState& a, double mx, double my, int width, int height) {
    a.dragging = false;

    if (a.mode == MODE_MENU) {
        int btn = menu_hit_test(mx, my, width, height);
        if (btn == 1)      app_enter_pregame(a);
        else if (btn == 2) {
#ifndef __EMSCRIPTEN__
            std::exit(0);  // Desktop quit
#endif
        }
        else if (btn == 3) app_enter_challenge_select(a);
        return;
    }

    if (a.mode == MODE_PREGAME) {
        // End-of-drag takes precedence over button-click handling so a
        // tiny slider wiggle isn't mistaken for a button press.
        if (a.slider_dragging) {
            a.slider_dragging = false;
            a.stockfish_elo = slider_px_to_elo(mx, width);
            queue_redraw(a);
            return;
        }
        int tc_index = -1;
        int btn = pregame_hit_test(mx, my, width, height,
                                   a.pregame_tc_open, &tc_index);
        if (a.pregame_tc_open) {
            // Dropdown is modal while open.
            if (btn == 6 && tc_index >= 0 && tc_index < TC_COUNT) {
                // Row click → select and collapse.
                a.time_control = static_cast<TimeControl>(tc_index);
                a.pregame_tc_open = false;
                a.pregame_tc_hover = -1;
                queue_redraw(a);
            } else if (btn == 5) {
                // Head click while open → collapse without change.
                a.pregame_tc_open = false;
                a.pregame_tc_hover = -1;
                queue_redraw(a);
            } else {
                // Click anywhere else → collapse without change.
                a.pregame_tc_open = false;
                a.pregame_tc_hover = -1;
                queue_redraw(a);
            }
            return;
        }
        if (btn == 1) {           // Start
            app_enter_game(a);
        } else if (btn == 2) {    // Back
            app_enter_menu(a);
        } else if (btn == 3) {    // Toggle side
            a.human_plays_white = !a.human_plays_white;
            queue_redraw(a);
        } else if (btn == 4) {    // Click on slider (not a drag)
            a.stockfish_elo = slider_px_to_elo(mx, width);
            queue_redraw(a);
        } else if (btn == 5) {    // Dropdown head → expand
            a.pregame_tc_open = true;
            a.pregame_tc_hover = -1;
            queue_redraw(a);
        }
        return;
    }

    if (a.mode == MODE_CHALLENGE_SELECT) {
        int idx = challenge_select_hit_test(
            mx, my, width, height, a.challenge_names);
        if (idx == -2)      app_enter_menu(a);
        else if (idx >= 0)  app_enter_challenge(a, idx);
        return;
    }

    if (a.mode == MODE_CHALLENGE && a.challenge_show_summary) {
        app_enter_menu(a);
        return;
    }

    if (a.mode == MODE_CHALLENGE && a.challenge_solved && !a.transition_active) {
        if (next_button_hit_test(mx, my, width, height)) {
            int next = a.current_challenge.current_index + 1;
            if (next < static_cast<int>(a.current_challenge.fens.size())) {
                a.transition_pending_next = next;
            } else {
                a.challenge_show_summary = true;
            }
            queue_redraw(a);
        }
        return;
    }

    // Try Again: reset the puzzle to its starting FEN. Only hit-test
    // once the shake has settled — the button isn't drawn during the
    // shake so clicks in that window shouldn't register.
    if (a.mode == MODE_CHALLENGE && a.challenge_mistake &&
        a.board_shake_x == 0.0f) {
        if (try_again_button_hit_test(mx, my, width, height)) {
            app_reset_challenge_puzzle(a);
            queue_redraw(a);
        }
        return;
    }

    // Withdraw confirmation modal is modal — it eats every click while
    // open, whether it hits a button or not.
    if (a.mode == MODE_PLAYING && a.withdraw_confirm_open) {
        double dx_m = mx - a.press_x, dy_m = my - a.press_y;
        if (dx_m*dx_m + dy_m*dy_m < 25.0) {
            int which = 0;
            withdraw_confirm_hit_test(mx, my, width, height, &which);
            if (which == 1) {            // Yes → back to main menu
                a.withdraw_confirm_open = false;
                a.withdraw_hover = 0;
                app_enter_menu(a);
                return;
            } else if (which == 2) {     // No → close modal
                a.withdraw_confirm_open = false;
                a.withdraw_hover = 0;
                queue_redraw(a);
            }
        }
        return;  // Always swallow clicks while the modal is open.
    }

    // "Back to Menu" button is live during both game-over and analysis
    // mode (entered via the withdraw flag). Analysis mode additionally
    // gets a "Continue Playing" button that exits analysis and resumes
    // the live game — same effect as pressing ESC.
    if (a.mode == MODE_PLAYING &&
        (a.game.game_over || a.game.analysis_mode)) {
        double dx_eg = mx - a.press_x, dy_eg = my - a.press_y;
        if (dx_eg*dx_eg + dy_eg*dy_eg < 25.0) {
            if (endgame_menu_button_hit_test(mx, my, width, height)) {
                app_enter_menu(a);
                return;
            }
            if (a.game.analysis_mode &&
                analysis_continue_button_hit_test(mx, my, width, height)) {
                game_exit_analysis(a.game);
                a.continue_playing_hover = false;
                a.endgame_menu_hover = false;
                refresh_play_status(a);
                queue_redraw(a);
                return;
            }
        }
        // In analysis mode, clicks that don't hit a button should
        // still not act on the board — fall through to camera drag.
        if (a.game.analysis_mode) return;
        // Fall through so camera-drag releases still work on game-over.
    }

    // Withdraw flag (live game only, no modal already open).
    if (a.mode == MODE_PLAYING && !a.game.game_over &&
        !a.game.analysis_mode && !a.withdraw_confirm_open) {
        double dx_f = mx - a.press_x, dy_f = my - a.press_y;
        if (dx_f*dx_f + dy_f*dy_f < 25.0 &&
            flag_hit_test(a.flag, mx, my, width, height)) {
            a.withdraw_confirm_open = true;
            a.withdraw_hover = 0;
            queue_redraw(a);
            return;
        }
    }

    // Regular game / challenge board interaction: only treat as a click
    // if the pointer didn't move much between press and release.
    double dx = mx - a.press_x, dy = my - a.press_y;
    if (dx*dx + dy*dy < 25.0) handle_board_click(a, mx, my, width, height);
}

void app_motion(AppState& a, double mx, double my, int width, int height) {
    if (a.mode == MODE_MENU) {
        int h = menu_hit_test(mx, my, width, height);
        if (h != a.menu_hover) {
            a.menu_hover = h;
            queue_redraw(a);
        }
        return;
    }
    if (a.mode == MODE_PREGAME) {
        int tc_idx = -1;
        int h = pregame_hit_test(mx, my, width, height,
                                 a.pregame_tc_open, &tc_idx);
        if (h != a.pregame_hover) {
            a.pregame_hover = h;
            queue_redraw(a);
        }
        // Dropdown hover: -2 for the head, 0..TC_COUNT-1 for a row,
        // -1 otherwise. Used by the renderer to tint the head and
        // the hovered row.
        int new_tc_hover = -1;
        if (a.pregame_tc_open) {
            if (h == 6) new_tc_hover = tc_idx;
            else if (h == 5) new_tc_hover = -2;
        } else if (h == 5) {
            new_tc_hover = -2;
        }
        if (new_tc_hover != a.pregame_tc_hover) {
            a.pregame_tc_hover = new_tc_hover;
            queue_redraw(a);
        }
        // If the mouse button is pressed AND the press was inside the
        // slider, treat this as a drag. Skip this while the dropdown
        // is open — it's modal.
        if (a.dragging && !a.pregame_tc_open) {
            if (!a.slider_dragging) {
                int start = pregame_hit_test(a.press_x, a.press_y,
                                             width, height,
                                             false, nullptr);
                if (start == 4) a.slider_dragging = true;
            }
            if (a.slider_dragging) {
                a.stockfish_elo = slider_px_to_elo(mx, width);
                queue_redraw(a);
            }
        }
        return;
    }
    if (a.mode == MODE_CHALLENGE_SELECT) {
        int h = challenge_select_hit_test(
            mx, my, width, height, a.challenge_names);
        if (h != a.challenge_select_hover) {
            a.challenge_select_hover = h;
            queue_redraw(a);
        }
        return;
    }
    if (a.mode == MODE_CHALLENGE && a.challenge_solved) {
        bool h_now = next_button_hit_test(mx, my, width, height);
        if (h_now != a.challenge_next_hover) {
            a.challenge_next_hover = h_now;
            queue_redraw(a);
        }
    }
    if (a.mode == MODE_CHALLENGE && a.challenge_mistake &&
        a.board_shake_x == 0.0f) {
        bool h_now = try_again_button_hit_test(mx, my, width, height);
        if (h_now != a.challenge_try_again_hover) {
            a.challenge_try_again_hover = h_now;
            queue_redraw(a);
        }
    }
    if (a.mode == MODE_PLAYING && a.withdraw_confirm_open) {
        int which = 0;
        withdraw_confirm_hit_test(mx, my, width, height, &which);
        if (which != a.withdraw_hover) {
            a.withdraw_hover = which;
            queue_redraw(a);
        }
        return;  // Modal swallows motion — no camera drag, no hover on
                 // other widgets.
    }
    if (a.mode == MODE_PLAYING &&
        (a.game.game_over || a.game.analysis_mode)) {
        bool h_now = endgame_menu_button_hit_test(mx, my, width, height);
        if (h_now != a.endgame_menu_hover) {
            a.endgame_menu_hover = h_now;
            queue_redraw(a);
        }
        if (a.game.analysis_mode) {
            bool c_now = analysis_continue_button_hit_test(mx, my, width, height);
            if (c_now != a.continue_playing_hover) {
                a.continue_playing_hover = c_now;
                queue_redraw(a);
            }
        } else if (a.continue_playing_hover) {
            a.continue_playing_hover = false;
            queue_redraw(a);
        }
    }
    if (a.dragging) {
        a.rot_y += static_cast<float>(mx - a.last_mouse_x) * 0.3f;
        a.rot_x += static_cast<float>(my - a.last_mouse_y) * 0.3f;
        if (a.rot_x < 5.0f)  a.rot_x = 5.0f;
        if (a.rot_x > 89.0f) a.rot_x = 89.0f;
        a.last_mouse_x = mx;
        a.last_mouse_y = my;
        queue_redraw(a);
    }
}

void app_scroll(AppState& a, double delta) {
    a.zoom += static_cast<float>(delta) * 0.5f;
    if (a.zoom < 3.0f)  a.zoom = 3.0f;
    if (a.zoom > 40.0f) a.zoom = 40.0f;
    queue_redraw(a);
}

void app_key(AppState& a, AppKey key) {
    GameState& gs = a.game;

    // First key in a menu-ish mode acts as the user gesture browsers
    // require before audio can start. Idempotent — no-op if already
    // looping.
    if (a.mode == MODE_MENU ||
        a.mode == MODE_PREGAME ||
        a.mode == MODE_CHALLENGE_SELECT) {
        audio_music_play("intro_music.wav");
    }

    // Modal takes priority over every other key handler — ESC closes
    // it, everything else is swallowed.
    if (a.mode == MODE_PLAYING && a.withdraw_confirm_open) {
        if (key == KEY_ESCAPE) {
            a.withdraw_confirm_open = false;
            a.withdraw_hover = 0;
            queue_redraw(a);
        }
        return;
    }

    // Dropdown-open is also modal — ESC collapses it.
    if (a.mode == MODE_PREGAME && a.pregame_tc_open) {
        if (key == KEY_ESCAPE) {
            a.pregame_tc_open = false;
            a.pregame_tc_hover = -1;
            queue_redraw(a);
        }
        return;
    }

    // Cartoon-outline toggle. Works in any in-game context
    // (playing or challenge), including analysis mode — it's a
    // pure render effect and doesn't interact with the rules.
    if (key == KEY_S &&
        (a.mode == MODE_PLAYING || a.mode == MODE_CHALLENGE)) {
        a.cartoon_outline = !a.cartoon_outline;
        queue_redraw(a);
        return;
    }

    if (a.mode == MODE_CHALLENGE) {
        if (key == KEY_ESCAPE) { app_reset_challenge_puzzle(a); return; }
        if (key == KEY_M)      { app_enter_menu(a); return; }
        return;
    }
    if (a.mode == MODE_CHALLENGE_SELECT) {
        if (key == KEY_ESCAPE) app_enter_menu(a);
        return;
    }

    if (gs.analysis_mode) {
        if (key == KEY_LEFT && gs.analysis_index > 0) {
            gs.analysis_index--;
            gs.restore_snapshot(gs.analysis_index);
            refresh_analysis_status(a);
            queue_redraw(a);
        } else if (key == KEY_RIGHT &&
                   gs.analysis_index <
                       static_cast<int>(gs.snapshots.size()) - 1) {
            gs.analysis_index++;
            gs.restore_snapshot(gs.analysis_index);
            refresh_analysis_status(a);
            queue_redraw(a);
        } else if (key == KEY_ESCAPE) {
            game_exit_analysis(gs);
            refresh_play_status(a);
            queue_redraw(a);
        }
        return;
    }

    if (key == KEY_A || key == KEY_LEFT || key == KEY_RIGHT) {
        if (!gs.ai_thinking && !gs.ai_animating && gs.snapshots.size() > 1) {
            game_enter_analysis(gs);
            if (key == KEY_LEFT && gs.analysis_index > 0)
                gs.analysis_index--;
            gs.restore_snapshot(gs.analysis_index);
            refresh_analysis_status(a);
            queue_redraw(a);
        }
    }
}

// ===========================================================================
// AI move animation + async result delivery
// ===========================================================================
static bool is_legal_ai_move(const AppState& a, int fc, int fr, int tc, int tr) {
    GameState& gs = const_cast<GameState&>(a.game);
    int idx = gs.grid[fr][fc];
    // AI's piece is whichever color the human isn't playing.
    if (idx < 0 || gs.pieces[idx].is_white == a.human_plays_white) return false;
    auto legal = generate_legal_moves(gs, fc, fr);
    for (const auto& [mc, mr] : legal)
        if (mc == tc && mr == tr) return true;
    return false;
}

static void start_ai_animation(AppState& a, int fc, int fr, int tc, int tr) {
    GameState& gs = a.game;
    gs.ai_from_col = fc;
    gs.ai_from_row = fr;
    gs.ai_to_col   = tc;
    gs.ai_to_row   = tr;
    gs.ai_animating = true;
    a.ai_anim_start_us = now_us(a);
    gs.ai_anim_start = a.ai_anim_start_us;
    queue_redraw(a);
}

static void ai_random_fallback(AppState& a) {
    GameState& gs = a.game;
    std::vector<std::pair<std::pair<int,int>, std::pair<int,int>>> all_legal;
    for (const auto& p : gs.pieces) {
        // AI moves whichever color the human isn't playing.
        if (!p.alive || p.is_white == a.human_plays_white) continue;
        auto moves = generate_legal_moves(gs, p.col, p.row);
        for (const auto& [mc, mr] : moves)
            all_legal.push_back({{p.col, p.row}, {mc, mr}});
    }
    if (all_legal.empty()) {
        gs.ai_thinking = false;
        app_refresh_status(a);
        queue_redraw(a);
        return;
    }
    int64_t t = now_us(a);
    int idx = static_cast<int>(static_cast<uint64_t>(t) % all_legal.size());
    auto& [from, to] = all_legal[idx];
    std::printf("AI fallback: %s\n",
                move_to_uci(from.first, from.second,
                            to.first,   to.second).c_str());
    start_ai_animation(a, from.first, from.second, to.first, to.second);
}

void app_ai_move_ready(AppState& a, const char* uci_c) {
    GameState& gs = a.game;

    // If we're not waiting for a move (e.g. user reset), drop the result.
    if (!gs.ai_thinking || gs.ai_animating) return;

    std::string uci = (uci_c && *uci_c) ? uci_c : "";
    int fc, fr, tc, tr;
    bool ok = false;
    if (!uci.empty() && parse_uci_move(uci, fc, fr, tc, tr)) {
        if (is_legal_ai_move(a, fc, fr, tc, tr)) ok = true;
    }

    if (ok) {
        std::printf("AI plays: %s (legal)\n", uci.c_str());
        start_ai_animation(a, fc, fr, tc, tr);
    } else {
        if (!uci.empty())
            std::fprintf(stderr, "AI move %s rejected; falling back\n",
                         uci.c_str());
        ai_random_fallback(a);
    }
}

void app_eval_ready(AppState& a, int cp, int score_index) {
    GameState& gs = a.game;
    if (cp == INT_MIN) return;
    if (score_index < 0 ||
        score_index >= static_cast<int>(gs.score_history.size())) return;

    // Collapse mate scores to a bounded ±100 spike for display.
    int mate_threshold = 30000 - 100;
    float pawn_units;
    if (cp >= mate_threshold) {
        pawn_units = 100.0f - static_cast<float>(30000 - cp);
    } else if (cp <= -mate_threshold) {
        pawn_units = -(100.0f - static_cast<float>(30000 + cp));
    } else {
        pawn_units = cp / 100.0f;
    }
    gs.score_history[score_index] = pawn_units;
    queue_redraw(a);
}

// ===========================================================================
// Per-frame tick
// ===========================================================================
void app_tick(AppState& a) {
    GameState& gs = a.game;
    int64_t now = now_us(a);

    // Keep the menu music queue topped up so it loops seamlessly.
    // audio_music_tick is a no-op when no track is playing, so it's
    // safe to call every frame in every mode.
    audio_music_tick();

    // Menu physics
    if (a.mode == MODE_MENU) {
        float dt = static_cast<float>(
            static_cast<double>(now - a.menu_last_update_us) / 1e6);
        a.menu_last_update_us = now;
        if (dt < 0.0f)     dt = 0.0f;
        if (dt > 0.05f)    dt = 0.05f;
        menu_update_physics(a.menu_pieces, dt);
        queue_redraw(a);
    }

    // AI move animation: when elapsed >= duration, commit the move.
    if (gs.ai_animating) {
        float elapsed =
            static_cast<float>(static_cast<double>(now - a.ai_anim_start_us) / 1e6);
        if (elapsed >= gs.ai_anim_duration) {
            gs.ai_animating = false;
            bool sfx_capture = gs.grid[gs.ai_to_row][gs.ai_to_col] >= 0;
            execute_move(gs, gs.ai_from_col, gs.ai_from_row,
                         gs.ai_to_col,   gs.ai_to_row);
            play_move_sfx(gs, sfx_capture);
            gs.ai_thinking = false;
            app_refresh_status(a);
            // Refresh the score graph for the position after the AI move.
            if (a.mode == MODE_PLAYING) {
                trigger_eval(a, static_cast<int>(gs.score_history.size()) - 1);
            }
        }
        queue_redraw(a);
    }

    // Selection ring pulses while a piece is selected — renderer reads
    // gs.anim_start_time directly, but we need redraws to animate it.
    if (gs.selected_col >= 0) queue_redraw(a);

    // Shatter transition has its own elapsed check in the render path;
    // we just need to keep issuing redraws while it's active.
    if (a.transition_active) queue_redraw(a);

    // Mistake shake: damped sine in view-space x. Settles to 0 after
    // ~0.7 s, at which point the Try Again button takes over.
    if (a.mode == MODE_CHALLENGE && a.challenge_mistake) {
        float t = static_cast<float>(
            static_cast<double>(now - a.challenge_mistake_start_us) / 1e6);
        constexpr float SHAKE_DURATION = 0.7f;
        if (t < SHAKE_DURATION) {
            float pi = static_cast<float>(M_PI);
            a.board_shake_x =
                0.25f * std::exp(-5.0f * t) * std::sin(2.0f * pi * 5.0f * t);
            queue_redraw(a);
        } else if (a.board_shake_x != 0.0f) {
            a.board_shake_x = 0.0f;
            queue_redraw(a);
        }
    }

    // Withdraw flag cloth physics: run during a live game (not
    // game-over, not analysis, not a paused modal). The flag itself
    // is (re)initialised in renderer_draw when it first sees the
    // window size, so this block is safe even on the very first tick
    // after entering the game.
    if (a.mode == MODE_PLAYING && !gs.game_over && !gs.analysis_mode &&
        !a.withdraw_confirm_open && !a.flag.p.empty()) {
        float dt;
        if (a.flag_last_update_us == 0) {
            dt = 0.0f;
        } else {
            dt = static_cast<float>(
                static_cast<double>(now - a.flag_last_update_us) / 1e6);
        }
        a.flag_last_update_us = now;
        if (dt < 0.0f)  dt = 0.0f;
        if (dt > 0.02f) dt = 0.02f;
        float time_s = static_cast<float>(
            static_cast<double>(now - a.flag_start_us) / 1e6);
        flag_update(a.flag, dt, time_s);
        queue_redraw(a);
    } else if (a.withdraw_confirm_open || gs.game_over || gs.analysis_mode) {
        // Pause the clock so dt after the modal closes is one frame,
        // not "however long the user stared at the dialog".
        a.flag_last_update_us = 0;
    }

    // Chess clock. Tick the side-to-move's budget, add Fischer
    // increment on turn flips, and set game_over on timeout. Same
    // pause-on-modal pattern as the flag so that reopening a modal
    // doesn't dump a huge dt into a clock.
    if (a.mode == MODE_PLAYING && a.clock_enabled && !gs.game_over &&
        !gs.analysis_mode && !a.withdraw_confirm_open) {
        if (a.clock_last_tick_us == 0) {
            a.clock_last_tick_us = now;
            a.prev_white_turn = gs.white_turn ? 1 : 0;
        } else {
            int64_t dt_us = now - a.clock_last_tick_us;
            a.clock_last_tick_us = now;
            int cur = gs.white_turn ? 1 : 0;
            // Charge this interval to whoever WAS on move. If the
            // turn flipped between ticks, the move happened
            // mid-interval — the dt belongs to the previous side
            // (they thought, then moved), not the new side.
            if (dt_us > 0) {
                int64_t dt_ms = dt_us / 1000;
                if (a.prev_white_turn == 1) a.white_ms_left -= dt_ms;
                else                        a.black_ms_left -= dt_ms;
            }
            // Turn flip → the side that just moved gets the
            // Fischer increment. Detected here rather than inside
            // execute_move so the rules layer stays pure.
            //
            // Cap each clock at the initial budget so the displayed
            // time never grows above the starting value (e.g. 30:00
            // in Classical). Raw Fischer behaviour would let a fast
            // player accumulate time above the cap, which combined
            // with our adaptive AI move time made the clock visibly
            // count UP during a classical game — surprising and
            // hard to reason about. Capping preserves the
            // incentive (play fast and you lose less time) without
            // the "growing clock" artefact.
            const int64_t base = TIME_CONTROLS[a.time_control].base_ms;
            if (cur != a.prev_white_turn) {
                int64_t inc = TIME_CONTROLS[a.time_control].increment_ms;
                if (a.prev_white_turn == 1) {
                    a.white_ms_left += inc;
                    if (a.white_ms_left > base) a.white_ms_left = base;
                } else {
                    a.black_ms_left += inc;
                    if (a.black_ms_left > base) a.black_ms_left = base;
                }
            }
            a.prev_white_turn = cur;
            // Time loss.
            if (a.white_ms_left <= 0) {
                a.white_ms_left = 0;
                gs.game_over = true;
                gs.game_result = "Black wins on time!";
                std::printf("Black wins on time!\n");
                app_refresh_status(a);
            } else if (a.black_ms_left <= 0) {
                a.black_ms_left = 0;
                gs.game_over = true;
                gs.game_result = "White wins on time!";
                std::printf("White wins on time!\n");
                app_refresh_status(a);
            }
        }
        queue_redraw(a);
    } else if (a.clock_enabled && (a.withdraw_confirm_open ||
                                   gs.game_over ||
                                   gs.analysis_mode)) {
        // Pause the clock — re-latch on the next active tick.
        a.clock_last_tick_us = 0;
    }
}

// ===========================================================================
// Rendering dispatch
// ===========================================================================
void app_render(AppState& a, int width, int height) {
    GameState& gs = a.game;
    int64_t now = now_us(a);

    if (a.mode == MODE_MENU) {
        float t = static_cast<float>(
            static_cast<double>(now - a.menu_start_time_us) / 1e6);
        renderer_draw_menu(a.menu_pieces, width, height, t, a.menu_hover);
        return;
    }

    if (a.mode == MODE_PREGAME) {
        renderer_draw_pregame(a.human_plays_white, a.stockfish_elo,
                              APP_ELO_MIN, APP_ELO_MAX,
                              a.time_control,
                              a.pregame_tc_open,
                              a.pregame_tc_hover,
                              width, height, a.pregame_hover);
        return;
    }

    if (a.mode == MODE_CHALLENGE_SELECT) {
        renderer_draw_challenge_select(
            a.challenge_names, width, height, a.challenge_select_hover);
        return;
    }

    if (a.mode == MODE_CHALLENGE && a.challenge_show_summary) {
        std::vector<SummaryEntry> entries;
        for (size_t i = 0; i < a.challenge_solutions.size(); i++) {
            SummaryEntry e;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "Puzzle %zu", i + 1);
            e.puzzle_name = buf;
            e.moves = a.challenge_solutions[i];
            entries.push_back(e);
        }
        renderer_draw_challenge_summary(
            a.current_challenge.name, entries, width, height);
        return;
    }

    // In-game / challenge board render path.

    // Suppress the desktop "wins by checkmate" overlay during challenge
    // mode — the challenge-specific "Next" button replaces it.
    bool save_game_over = false;
    std::string save_result;
    if (a.mode == MODE_CHALLENGE) {
        save_game_over = gs.game_over;
        save_result    = gs.game_result;
        gs.game_over = false;
        gs.game_result.clear();
    }

    // (Re)initialise the withdraw flag when the window size changes
    // or on the very first draw of a new game. Cheap (O(N) in grid
    // cells) so we don't need to guard against extra re-inits.
    if (a.mode == MODE_PLAYING &&
        (a.flag.inited_w != width || a.flag.inited_h != height)) {
        flag_init(a.flag, width, height);
        a.flag_last_update_us = 0;
    }

    const bool draw_flag =
        a.mode == MODE_PLAYING &&
        !gs.game_over && !gs.analysis_mode && !a.withdraw_confirm_open;

    // Clock widget: same visibility gate as the flag, plus the
    // time control must be non-Unlimited. The side shown is whoever
    // is on move — matches the user's spec ("when it's white's turn
    // it shows white's clock, etc.").
    const bool draw_clock =
        a.mode == MODE_PLAYING && a.clock_enabled &&
        !gs.game_over && !gs.analysis_mode && !a.withdraw_confirm_open;
    int64_t clock_ms = gs.white_turn ? a.white_ms_left : a.black_ms_left;
    bool clock_side_is_white = gs.white_turn;

    renderer_draw(gs, width, height, a.rot_x, a.rot_y, a.zoom,
                  a.human_plays_white,
                  a.endgame_menu_hover, a.continue_playing_hover,
                  &a.flag, draw_flag,
                  a.withdraw_confirm_open, a.withdraw_hover,
                  draw_clock, clock_ms, clock_side_is_white,
                  a.cartoon_outline,
                  a.board_shake_x);

    if (a.mode != MODE_CHALLENGE) return;

    // Restore game_over before any further drawing.
    gs.game_over   = save_game_over;
    gs.game_result = save_result;

    renderer_draw_challenge_overlay(
        a.current_challenge.name,
        a.current_challenge.current_index,
        static_cast<int>(a.current_challenge.fens.size()),
        a.challenge_moves_made,
        a.current_challenge.max_moves,
        a.current_challenge.starts_white,
        width, height);

    if (a.challenge_solved && !a.transition_active &&
        a.transition_pending_next < 0) {
        renderer_draw_next_button(width, height, a.challenge_next_hover);
    }

    // Try Again button only appears once the board shake has finished,
    // so the mistake feedback has a clear "then" beat to it.
    if (a.challenge_mistake && a.board_shake_x == 0.0f) {
        renderer_draw_try_again_button(width, height, a.challenge_try_again_hover);
    }

    // Transition trigger: capture current frame, load next puzzle,
    // redraw the new state, then the shatter overlay animates the
    // captured texture fading away.
    if (a.transition_pending_next >= 0) {
        renderer_capture_frame(width, height);
        app_load_challenge_puzzle(a, a.transition_pending_next);
        a.transition_pending_next = -1;
        a.transition_active = true;
        a.transition_start_time_us = now;
        audio_play(SoundEffect::GlassBreak);

        // The renderer_draw path itself clears its color/depth buffers.
        // Challenge mode never wants the withdraw flag, modal, or clock.
        // Reuse the session-level cartoon_outline setting so toggling
        // it survives across a puzzle transition.
        renderer_draw(gs, width, height, a.rot_x, a.rot_y, a.zoom,
                      a.human_plays_white,
                      a.endgame_menu_hover, false,
                      nullptr, false, false, 0,
                      false, 0, false,
                      a.cartoon_outline);
        renderer_draw_challenge_overlay(
            a.current_challenge.name,
            a.current_challenge.current_index,
            static_cast<int>(a.current_challenge.fens.size()),
            a.challenge_moves_made,
            a.current_challenge.max_moves,
            a.current_challenge.starts_white,
            width, height);
    }

    if (a.transition_active) {
        float t = static_cast<float>(
            static_cast<double>(now - a.transition_start_time_us) / 1e6);
        static constexpr float TRANSITION_DURATION = 1.3f;
        if (t >= TRANSITION_DURATION) {
            a.transition_active = false;
        } else {
            renderer_draw_shatter(t, width, height);
        }
    }
}

// ===========================================================================
// Lifecycle
// ===========================================================================
void app_init(AppState& a, const AppPlatform* platform) {
    a.platform = platform;
    game_reset(a.game);
}
