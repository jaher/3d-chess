#include "app_state.h"

#include "ai_player.h"
#include "audio.h"
#include "chess_rules.h"
#include "cloth_flag.h"
#include "mat.h"
#include "voice_input.h"
#include "voice_tts.h"
#include "chessnut_encode.h"
#include "phantom_encode.h"
#ifndef __EMSCRIPTEN__
#  include "chessnut_bridge.h"
#  include "phantom_bridge.h"
#endif

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <memory>
#include <utility>
#include <vector>

// ===========================================================================
// Small helpers
// ===========================================================================
namespace {

int64_t now_us(const AppState& a) {
    return a.platform ? a.platform->now_us() : 0;
}

// Mistake board-shake duration (seconds). Also the floor for the
// Try-Again reveal delay — the button never surfaces before the shake
// has settled, even if the sfx is shorter than this.
constexpr float MISTAKE_SHAKE_DURATION = 0.7f;

// The Try-Again button should surface only after BOTH the shake has
// settled and the mistake sfx has played out. Returns false when no
// mistake is active, or while either is still in flight.
bool mistake_reveal_ready(const AppState& a) {
    if (!a.challenge_mistake) return false;
    float elapsed = static_cast<float>(
        static_cast<double>(now_us(a) - a.challenge_mistake_start_us) / 1e6);
    float sfx = audio_clip_duration_seconds(SoundEffect::Mistake);
    float reveal_at = MISTAKE_SHAKE_DURATION > sfx ? MISTAKE_SHAKE_DURATION : sfx;
    return elapsed >= reveal_at;
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
        gs.castling.black_rook_a_moved, gs.castling.black_rook_h_moved,
        gs.ep_target_col, gs.ep_target_row);
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
            gs.game_over)
            return;
        // In two-player (Chessnut hot-seat) mode, the user clicks
        // for whichever side's turn it is. In single-player mode,
        // clicks for the AI side are ignored.
        if (!a.two_player_mode &&
            gs.white_turn != a.human_plays_white)
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
                // that info is gone once execute_move runs. Same
                // snapshot powers the TTS render below.
                bool sfx_capture = gs.grid[row][col] >= 0;
                BoardSnapshot tts_before;
                if (a.voice_tts_enabled) {
                    tts_before.pieces        = gs.pieces;
                    tts_before.white_turn    = gs.white_turn;
                    tts_before.castling      = gs.castling;
                    tts_before.ep_target_col = gs.ep_target_col;
                    tts_before.ep_target_row = gs.ep_target_row;
                }
                execute_move(gs, gs.selected_col, gs.selected_row, col, row);
                gs.selected_col = gs.selected_row = -1;
                gs.valid_moves.clear();
                gs.hint_from_col = gs.hint_from_row = -1;
                gs.hint_to_col   = gs.hint_to_row   = -1;
                a.hint_confirm_pending = false;
                play_move_sfx(gs, sfx_capture);
                if (a.voice_tts_enabled && !gs.move_history.empty()) {
                    voice_tts_speak(uci_to_speech(
                        tts_before, gs.move_history.back()));
                }
                queue_redraw(a);
                app_chessnut_sync_board(a, /*force=*/false);

                if (is_challenge) {
                    if (was_starter) a.challenge_moves_made++;
                    // Append the just-played move's algebraic notation
                    // to challenge_solutions[current_index] for the
                    // end-of-challenge summary table. No-op if anything
                    // unexpected (empty history, missing snapshot,
                    // out-of-range index).
                    auto record_solution = [&]() {
                        if (gs.move_history.empty() ||
                            gs.snapshots.size() < 2) return;
                        int pi = a.current_challenge.current_index;
                        if (pi < 0 ||
                            pi >= static_cast<int>(a.challenge_solutions.size()))
                            return;
                        const auto& before =
                            gs.snapshots[gs.snapshots.size() - 2];
                        a.challenge_solutions[pi].push_back(
                            uci_to_algebraic(before, gs.move_history.back()));
                    };
                    // mate_in_N records every attempt; tactics record
                    // only verified-correct candidates inside the
                    // is_new_correct branch below.
                    if (!is_tactic_type(a.current_challenge.type)) {
                        record_solution();
                    }
                    // Tactic puzzles (find_forks / find_pins): user
                    // must enumerate every legal move that creates the
                    // motif. Each attempt is checked against the
                    // precomputed required-move list. A new correct
                    // candidate auto-resets the board so the next one
                    // can be played; a wrong or duplicate move kicks
                    // off the same mistake flow as mate_in_N (shake +
                    // sfx + Try-Again button) without throwing away
                    // earlier correct candidates — found_moves is
                    // preserved across Try-Again clicks.
                    if (was_starter) {
                        const std::string& ct = a.current_challenge.type;
                        if (is_tactic_type(ct)) {
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
                            bool is_new_correct = is_required && !already_found;

                            char buf[160];
                            const char* noun =
                                (ct == "find_forks") ? "forks" : "pins";

                            // Cap at 3 candidates per puzzle so
                            // positions with many legal forks / pins
                            // don't turn into busywork. The full
                            // required_moves list is still used to
                            // validate / dedupe attempts; we just
                            // mark the puzzle solved (and show the
                            // progress fraction) against this target.
                            size_t tactic_target =
                                std::min<size_t>(3, req.size());
                            if (is_new_correct) {
                                found.push_back(uci);
                                record_solution();
                                int pi = a.current_challenge.current_index;
                                // Reset to the starting FEN for the
                                // next candidate. Force-sync the
                                // physical board so it follows the
                                // reset (apply_fen_to_state mutates
                                // gs without going through
                                // execute_move, so the per-move
                                // sync at line 462 wouldn't catch
                                // this).
                                ParsedFEN p = parse_fen(
                                    a.current_challenge.fens[pi]);
                                if (p.valid) {
                                    apply_fen_to_state(a.game, p);
                                    app_chessnut_sync_board(a, /*force=*/true);
                                }
                                a.challenge_moves_made = 0;
                                if (tactic_target > 0 &&
                                    found.size() >= tactic_target) {
                                    a.challenge_solved = true;
                                }
                                std::snprintf(
                                    buf, sizeof(buf),
                                    "Good! — found %d / %d %s",
                                    static_cast<int>(found.size()),
                                    static_cast<int>(tactic_target),
                                    noun);
                            } else {
                                // Wrong move OR a duplicate of an
                                // already-found candidate. Both count
                                // as a mistake here — replaying a
                                // known fork doesn't advance the
                                // puzzle. Don't auto-reset the board;
                                // let the shake play out and surface
                                // Try-Again. Preserved found_moves
                                // ride through app_reset_challenge_
                                // puzzle's save/restore.
                                if (!a.challenge_mistake) {
                                    a.challenge_mistake = true;
                                    a.challenge_mistake_start_us = now_us(a);
                                    a.challenge_try_again_hover = false;
                                    audio_play(SoundEffect::Mistake);
                                }
                                std::snprintf(
                                    buf, sizeof(buf),
                                    "Not a match — found %d / %d %s",
                                    static_cast<int>(found.size()),
                                    static_cast<int>(tactic_target),
                                    noun);
                            }
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
                    // Two-player mode: no AI replies — both sides
                    // are humans (one on the physical board, one on
                    // screen, or both via clicks).
                    if (!a.two_player_mode &&
                        gs.white_turn != a.human_plays_white &&
                        !gs.game_over)
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
        // Tactic puzzles: hide already-banked candidates from the
        // move dots so the user can't waste clicks on a fork / pin
        // they've already found. The same UCI in found_moves would
        // otherwise trigger the duplicate-as-mistake path.
        if (is_challenge && is_tactic_type(a.current_challenge.type) &&
            !a.current_challenge.found_moves.empty()) {
            const auto& found = a.current_challenge.found_moves;
            gs.valid_moves.erase(
                std::remove_if(
                    gs.valid_moves.begin(), gs.valid_moves.end(),
                    [&](const std::pair<int,int>& m) {
                        std::string uci = move_to_uci(col, row, m.first, m.second);
                        return std::find(found.begin(), found.end(), uci) !=
                               found.end();
                    }),
                gs.valid_moves.end());
        }
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
    a.menu_grabbed_piece = -1;
    a.withdraw_confirm_open = false;
    a.withdraw_hover = 0;
    a.endgame_menu_hover = false;
    a.continue_playing_hover = false;
    a.chessnut_missing_modal_open = false;
    a.chessnut_missing_squares_msg.clear();
    a.chessnut_missing_exit_hover = false;
    a.chessnut_pending_ai_trigger = false;
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
    std::fprintf(stderr,
        "[app] enter_game: two_player=%d human_plays_white=%d "
        "chessnut_connected=%d\n",
        a.two_player_mode ? 1 : 0,
        a.human_plays_white ? 1 : 0,
        a.chessnut_connected ? 1 : 0);
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
    // Drop the sensor-frame baseline so the next stable frame after
    // motors settle gets re-checked against the starting position.
    // Any squares still disagreeing then are missing pieces / dead
    // ID-chip batteries, and the user gets a status hint.
    a.chessnut_sensor_baseline_set = false;
    // First-time-on-start position sync. force=true so the firmware
    // always replans from current sensor state — handles boards
    // that were left in arbitrary positions after the last session.
    app_chessnut_sync_board(a, /*force=*/true);

    // While the motors are arranging the starting position on the
    // physical board, block game input with a "positioning" modal.
    // The modal auto-closes from the apply_sensor_frame baseline
    // path once a stable sensor frame confirms the layout matches
    // (or transitions to Missing / WrongLayout if it doesn't).
    bool ai_will_open_first = !a.two_player_mode && !a.human_plays_white;
    if (a.chessnut_connected) {
        a.chessnut_missing_modal_open = true;
        a.chessnut_missing_modal_type = AppState::ChessnutModalType::Positioning;
        a.chessnut_missing_squares_msg.clear();
        a.chessnut_missing_exit_hover = false;
        // Hold Stockfish's first move until motors finish — kicking
        // it off now would race the digital state ahead of the
        // physical board.
        a.chessnut_pending_ai_trigger = ai_will_open_first;
        queue_redraw(a);
    } else if (ai_will_open_first) {
        // No chessnut board — Stockfish goes immediately as before.
        trigger_ai(a);
    }
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

void app_enter_options(AppState& a) {
    a.mode = MODE_OPTIONS;
    a.options_hover = 0;
    set_status(a, "Options");
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

    // Fresh puzzle: drop tactic progress from any prior puzzle in
    // this Challenge. Try-Again preserves found_moves separately
    // by saving / restoring around app_load_challenge_puzzle.
    a.current_challenge.found_moves.clear();
    a.current_challenge.required_moves.clear();

    // For find_forks / find_pins puzzles, pre-compute every legal
    // move that produces the target motif so we can track progress
    // as the user plays them one at a time.
    const std::string& ct = a.current_challenge.type;
    if (is_tactic_type(ct)) {
        a.current_challenge.required_moves = find_tactic_moves(
            a.game, a.current_challenge.starts_white, ct
        );
        // Degenerate puzzle: no legal motif moves at all (bad data
        // or a position where no fork / pin is possible). Mark it
        // solved so the user can advance to the next exercise
        // instead of being stuck with the Next button hidden.
        if (a.current_challenge.required_moves.empty()) {
            a.challenge_solved = true;
        }
    }
    char buf[160];
    std::snprintf(buf, sizeof(buf), "Challenge: %s [%d/%d]",
                  a.current_challenge.name.c_str(),
                  puzzle_index + 1,
                  static_cast<int>(a.current_challenge.fens.size()));
    set_status(a, buf);
    queue_redraw(a);
    // Position the physical board to match the new puzzle's FEN.
    app_chessnut_sync_board(a, /*force=*/true);
}

void app_reset_challenge_puzzle(AppState& a) {
    int idx = a.current_challenge.current_index;
    bool is_tactic = is_tactic_type(a.current_challenge.type);
    // Try-Again rewinds the board but keeps tactic progress: earlier
    // correct fork / pin candidates stay banked in challenge_solutions
    // (for the summary) and in found_moves (for in-game progress).
    // mate_in_N wipes its solutions because a failed line is replayed
    // from scratch, not refined. found_moves still needs save/restore
    // because app_load_challenge_puzzle clears it; challenge_solutions
    // is left untouched by load so we just skip the clear.
    if (!is_tactic && idx >= 0 &&
        idx < static_cast<int>(a.challenge_solutions.size())) {
        a.challenge_solutions[idx].clear();
    }
    auto saved_found = std::move(a.current_challenge.found_moves);
    app_load_challenge_puzzle(a, idx);
    a.current_challenge.found_moves = std::move(saved_found);
    size_t tactic_target = std::min<size_t>(
        3, a.current_challenge.required_moves.size());
    if (tactic_target > 0 &&
        a.current_challenge.found_moves.size() >= tactic_target) {
        a.challenge_solved = true;
    }
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
    a.press_time_us = now_us(a);
    a.fling_sample_x = mx;
    a.fling_sample_y = my;
    a.fling_sample_time_us = a.press_time_us;
    // A previous gesture may have ended without a release event (e.g.
    // the pointer left the canvas). Drop the stale grab so the new
    // press starts a fresh hit-test in app_motion.
    a.menu_grabbed_piece = -1;

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

// ===========================================================================
// Per-mode release handlers
// ===========================================================================
static void release_menu(AppState& a, double mx, double my,
                         int width, int height) {
    // A button counts as clicked only when the cursor was on the
    // SAME button at press and at release — so dragging off a
    // button cancels its click, and flicking a piece that starts
    // under a button doesn't accidentally press it.
    int press_btn   = menu_hit_test(a.press_x, a.press_y,
                                    width, height, a.chessnut_connected);
    int release_btn = menu_hit_test(mx, my, width, height,
                                    a.chessnut_connected);
    if (press_btn != 0 && press_btn == release_btn) {
        a.menu_grabbed_piece = -1;
        if (press_btn == 1)      { a.two_player_mode = false; app_enter_pregame(a); }
        else if (press_btn == 3) app_enter_challenge_select(a);
        else if (press_btn == 4) app_enter_options(a);
        else if (press_btn == 5) { a.two_player_mode = true;  app_enter_pregame(a); }
#ifndef __EMSCRIPTEN__
        else if (press_btn == 2) {
            // Quit button — go through the platform hook so main()
            // returns and runs the cleanup chain (voice_tts /
            // chessnut / audio shutdown). Calling std::exit() here
            // jumps over that chain and into static-destructor land
            // where the still-joinable voice_tts worker thread
            // deadlocks the process.
            if (a.platform && a.platform->request_quit)
                a.platform->request_quit();
        }
#endif
        return;
    }
    // Press started on a button but released elsewhere (or vice
    // versa): cancelled click, no piece throw either.
    if (press_btn != 0 || release_btn != 0) {
        a.menu_grabbed_piece = -1;
        queue_redraw(a);
        return;
    }

    // Drag-to-fling: app_motion latched the grabbed piece on first
    // motion; a pure click without motion falls back to hit-testing
    // here. Release delta / dt over the rolling window gives the
    // throw velocity, so a slow drag followed by a fast flick still
    // launches hard.
    int64_t now = now_us(a);
    float t_s  = static_cast<float>(
        static_cast<double>(now - a.menu_start_time_us) / 1e6);
    int idx = a.menu_grabbed_piece;
    if (idx < 0) {
        idx = menu_piece_hit_test(a.menu_pieces,
                                  a.press_x, a.press_y,
                                  width, height, t_s);
    }
    if (idx >= 0) {
        float dt_s = static_cast<float>(
            static_cast<double>(now - a.fling_sample_time_us) / 1e6);
        menu_throw_piece(a.menu_pieces[idx],
                         a.fling_sample_x, a.fling_sample_y,
                         mx, my,
                         dt_s, width, height, t_s);
        audio_play(SoundEffect::Capture);
    }
    a.menu_grabbed_piece = -1;
    queue_redraw(a);
}

static void release_pregame(AppState& a, double mx, double my,
                            int width, int height) {
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
                               a.pregame_tc_open,
                               /*hide_elo_slider=*/a.two_player_mode,
                               &tc_index);
    if (a.pregame_tc_open) {
        // Dropdown is modal while open. Row click → select and
        // collapse; any other click → collapse without change.
        if (btn == 6 && tc_index >= 0 && tc_index < TC_COUNT) {
            a.time_control = static_cast<TimeControl>(tc_index);
        }
        a.pregame_tc_open = false;
        a.pregame_tc_hover = -1;
        queue_redraw(a);
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
}

static void release_challenge_select(AppState& a, double mx, double my,
                                     int width, int height) {
    int idx = challenge_select_hit_test(
        mx, my, width, height, a.challenge_names);
    if (idx == -2)      app_enter_menu(a);
    else if (idx >= 0)  app_enter_challenge(a, idx);
}

static void release_options(AppState& a, double mx, double my,
                            int width, int height) {
    bool voice_supported    = app_voice_continuous_supported();
    bool chessnut_supported = app_chessnut_supported();
    int btn = options_hit_test(mx, my, width, height,
                               voice_supported, chessnut_supported,
                               a.chessnut_picker_open,
                               static_cast<int>(a.chessnut_devices.size()));
    if (btn == 1) {
        app_enter_menu(a);
    } else if (btn == 2) {
        a.cartoon_outline = !a.cartoon_outline;
        app_settings_save(a);
        queue_redraw(a);
    } else if (btn == 3 && voice_supported) {
        app_voice_toggle_continuous_request(a);
    } else if (btn == 4 && chessnut_supported) {
        app_chessnut_toggle_request(a);
    } else if (btn == 5 && a.chessnut_picker_open) {
        app_chessnut_close_picker(a);
    } else if (btn == 6 && a.chessnut_picker_open) {
        app_chessnut_forget_cached_device(a);
    } else if (btn == 7 && chessnut_supported) {
        a.ble_verbose_log = !a.ble_verbose_log;
        set_status(a, a.ble_verbose_log
            ? "BLE verbose log: ON — every notify frame will surface here"
            : "BLE verbose log: OFF");
        queue_redraw(a);
    } else if (btn == 8 && voice_supported) {
        app_voice_toggle_speak_moves_request(a);
    } else if (btn == 9) {
        // Cycle Off → Auto → OnDemand → Off.
        switch (a.hint_mode) {
        case AppState::HintMode::Off:
            a.hint_mode = AppState::HintMode::Auto;
            set_status(a,
                "Move hints: AUTO — recommendations will appear "
                "in yellow on every turn");
            break;
        case AppState::HintMode::Auto:
            a.hint_mode = AppState::HintMode::OnDemand;
            set_status(a,
                "Move hints: ON DEMAND — say \"give me a hint\" "
                "to surface the recommendation");
            break;
        case AppState::HintMode::OnDemand:
            a.hint_mode = AppState::HintMode::Off;
            set_status(a, "Move hints: OFF");
            break;
        }
        // Clear any in-flight ring state when the mode changes so
        // a stale hint from the previous mode doesn't linger.
        a.game.hint_from_col = a.game.hint_from_row = -1;
        a.game.hint_to_col   = a.game.hint_to_row   = -1;
        a.game.hint_last_spoken_uci.clear();
        a.game.hint_last_spoken_ply = -1;
        a.hint_request_pending = false;
        a.hint_confirm_pending = false;
        queue_redraw(a);
    } else if (btn >= 100 && a.chessnut_picker_open) {
        int idx = btn - 100;
        if (idx >= 0 &&
            idx < static_cast<int>(a.chessnut_devices.size())) {
            app_chessnut_pick_device(a, a.chessnut_devices[idx].address);
        }
    }
}

static void release_challenge(AppState& a, double mx, double my,
                              int width, int height) {
    if (a.challenge_show_summary) {
        app_enter_menu(a);
        return;
    }
    if (a.challenge_solved && !a.transition_active) {
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
    // once the shake has settled AND the mistake sfx has played out
    // — the button isn't drawn before then, so clicks in that window
    // shouldn't register.
    if (mistake_reveal_ready(a)) {
        if (try_again_button_hit_test(mx, my, width, height)) {
            app_reset_challenge_puzzle(a);
            queue_redraw(a);
        }
        return;
    }
    // Challenge board click: fall through to the same handler the
    // live game uses (treat the click as a move attempt).
    double dx = mx - a.press_x, dy = my - a.press_y;
    if (dx*dx + dy*dy < 25.0) handle_board_click(a, mx, my, width, height);
}

static void release_playing(AppState& a, double mx, double my,
                            int width, int height) {
    // Pieces-missing modal eats every click while open. Only the
    // Exit-to-Menu button does anything; everywhere else the
    // click is swallowed.
    if (a.chessnut_missing_modal_open) {
        double dx_m = mx - a.press_x, dy_m = my - a.press_y;
        if (dx_m*dx_m + dy_m*dy_m < 25.0 &&
            chessnut_missing_exit_button_hit_test(mx, my, width, height)) {
            app_enter_menu(a);
        }
        return;
    }

    // Withdraw confirmation modal is modal — it eats every click
    // while open, whether it hits a button or not.
    if (a.withdraw_confirm_open) {
        double dx_m = mx - a.press_x, dy_m = my - a.press_y;
        if (dx_m*dx_m + dy_m*dy_m < 25.0) {
            int which = 0;
            withdraw_confirm_hit_test(mx, my, width, height, &which);
            if (which == 1) {            // Yes → back to main menu
                a.withdraw_confirm_open = false;
                a.withdraw_hover = 0;
                app_enter_menu(a);
                return;
            }
            if (which == 2) {            // No → close modal
                a.withdraw_confirm_open = false;
                a.withdraw_hover = 0;
                queue_redraw(a);
            }
        }
        return;
    }

    // "Back to Menu" button is live during both game-over and
    // analysis mode (entered via the withdraw flag). Analysis mode
    // additionally gets a "Continue Playing" button that exits
    // analysis and resumes the live game — same effect as ESC.
    if (a.game.game_over || a.game.analysis_mode) {
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
        // In analysis mode, non-button clicks still shouldn't act
        // on the board. Game-over falls through to camera-drag
        // release below.
        if (a.game.analysis_mode) return;
    }

    // Withdraw flag (live game only, no modal already open).
    if (!a.game.game_over && !a.game.analysis_mode) {
        double dx_f = mx - a.press_x, dy_f = my - a.press_y;
        if (dx_f*dx_f + dy_f*dy_f < 25.0 &&
            flag_hit_test(a.flag, mx, my, width, height)) {
            a.withdraw_confirm_open = true;
            a.withdraw_hover = 0;
            queue_redraw(a);
            return;
        }
    }

    // Board interaction: only treat as a click if the pointer
    // didn't move much between press and release.
    double dx = mx - a.press_x, dy = my - a.press_y;
    if (dx*dx + dy*dy < 25.0) handle_board_click(a, mx, my, width, height);
}

void app_release(AppState& a, double mx, double my, int width, int height) {
    a.dragging = false;
    switch (a.mode) {
        case MODE_MENU:             release_menu(a, mx, my, width, height); return;
        case MODE_PREGAME:          release_pregame(a, mx, my, width, height); return;
        case MODE_CHALLENGE_SELECT: release_challenge_select(a, mx, my, width, height); return;
        case MODE_OPTIONS:          release_options(a, mx, my, width, height); return;
        case MODE_CHALLENGE:        release_challenge(a, mx, my, width, height); return;
        case MODE_PLAYING:          release_playing(a, mx, my, width, height); return;
    }
}

// ===========================================================================
// Per-mode motion handlers
// ===========================================================================
// Orbit-camera drag used by MODE_PLAYING and MODE_CHALLENGE. Bounds
// rot_x to avoid flipping through the floor / ceiling.
static void apply_camera_drag(AppState& a, double mx, double my) {
    if (!a.dragging) return;
    a.rot_y += static_cast<float>(mx - a.last_mouse_x) * 0.3f;
    a.rot_x += static_cast<float>(my - a.last_mouse_y) * 0.3f;
    if (a.rot_x < 5.0f)  a.rot_x = 5.0f;
    if (a.rot_x > 89.0f) a.rot_x = 89.0f;
    a.last_mouse_x = mx;
    a.last_mouse_y = my;
    queue_redraw(a);
}

static void motion_menu(AppState& a, double mx, double my, int width, int height) {
    int h = menu_hit_test(mx, my, width, height, a.chessnut_connected);
    if (h != a.menu_hover) {
        a.menu_hover = h;
        queue_redraw(a);
    }
    if (!a.dragging) return;

    // First motion after press is our earliest chance to hit-test
    // (app_press has no viewport size). Once a piece is grabbed
    // the index stays until release.
    if (a.menu_grabbed_piece < 0) {
        float t_s = static_cast<float>(
            static_cast<double>(now_us(a) - a.menu_start_time_us) / 1e6);
        a.menu_grabbed_piece = menu_piece_hit_test(
            a.menu_pieces, a.press_x, a.press_y, width, height, t_s);
        if (a.menu_grabbed_piece >= 0) queue_redraw(a);
    }
    // Advance the fling reference only after a minimum gap (~60 ms)
    // so release velocity reflects the last leg of the drag, not
    // the whole gesture.
    constexpr int64_t FLING_WINDOW_US = 60'000;
    int64_t now = now_us(a);
    if (now - a.fling_sample_time_us >= FLING_WINDOW_US) {
        a.fling_sample_x = mx;
        a.fling_sample_y = my;
        a.fling_sample_time_us = now;
    }
}

static void motion_pregame(AppState& a, double mx, double my, int width, int height) {
    int tc_idx = -1;
    int h = pregame_hit_test(mx, my, width, height,
                             a.pregame_tc_open,
                             /*hide_elo_slider=*/a.two_player_mode, &tc_idx);
    if (h != a.pregame_hover) {
        a.pregame_hover = h;
        queue_redraw(a);
    }
    // Dropdown hover: -2 for the head, 0..TC_COUNT-1 for a row,
    // -1 otherwise. Drives the head/row tint in the renderer.
    int new_tc_hover = -1;
    if (a.pregame_tc_open) {
        if (h == 6)      new_tc_hover = tc_idx;
        else if (h == 5) new_tc_hover = -2;
    } else if (h == 5) {
        new_tc_hover = -2;
    }
    if (new_tc_hover != a.pregame_tc_hover) {
        a.pregame_tc_hover = new_tc_hover;
        queue_redraw(a);
    }
    // Mouse button held AND press was inside the slider → slider
    // drag. Skipped while the dropdown is open (it's modal).
    if (a.dragging && !a.pregame_tc_open) {
        if (!a.slider_dragging) {
            int start = pregame_hit_test(a.press_x, a.press_y,
                                         width, height,
                                         false, a.two_player_mode, nullptr);
            if (start == 4) a.slider_dragging = true;
        }
        if (a.slider_dragging) {
            a.stockfish_elo = slider_px_to_elo(mx, width);
            queue_redraw(a);
        }
    }
}

static void motion_challenge_select(AppState& a, double mx, double my,
                                    int width, int height) {
    int h = challenge_select_hit_test(
        mx, my, width, height, a.challenge_names);
    if (h != a.challenge_select_hover) {
        a.challenge_select_hover = h;
        queue_redraw(a);
    }
}

static void motion_options(AppState& a, double mx, double my,
                           int width, int height) {
    int h = options_hit_test(mx, my, width, height,
                             app_voice_continuous_supported(),
                             app_chessnut_supported(),
                             a.chessnut_picker_open,
                             static_cast<int>(a.chessnut_devices.size()));
    if (h != a.options_hover) {
        a.options_hover = h;
        queue_redraw(a);
    }
    int row = (h >= 100) ? (h - 100) : -1;
    if (row != a.chessnut_picker_hover) {
        a.chessnut_picker_hover = row;
        queue_redraw(a);
    }
}

static void motion_challenge(AppState& a, double mx, double my,
                             int width, int height) {
    if (a.challenge_solved) {
        bool h_now = next_button_hit_test(mx, my, width, height);
        if (h_now != a.challenge_next_hover) {
            a.challenge_next_hover = h_now;
            queue_redraw(a);
        }
    }
    if (mistake_reveal_ready(a)) {
        bool h_now = try_again_button_hit_test(mx, my, width, height);
        if (h_now != a.challenge_try_again_hover) {
            a.challenge_try_again_hover = h_now;
            queue_redraw(a);
        }
    }
    apply_camera_drag(a, mx, my);
}

static void motion_playing(AppState& a, double mx, double my,
                           int width, int height) {
    if (a.chessnut_missing_modal_open) {
        bool h_now = chessnut_missing_exit_button_hit_test(mx, my, width, height);
        if (h_now != a.chessnut_missing_exit_hover) {
            a.chessnut_missing_exit_hover = h_now;
            queue_redraw(a);
        }
        // Modal swallows the rest of the motion handling — no
        // camera drag, no hover on board widgets.
        return;
    }
    if (a.withdraw_confirm_open) {
        int which = 0;
        withdraw_confirm_hit_test(mx, my, width, height, &which);
        if (which != a.withdraw_hover) {
            a.withdraw_hover = which;
            queue_redraw(a);
        }
        // Modal swallows motion — no camera drag, no hover on
        // other widgets.
        return;
    }
    if (a.game.game_over || a.game.analysis_mode) {
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
    apply_camera_drag(a, mx, my);
}

void app_motion(AppState& a, double mx, double my, int width, int height) {
    switch (a.mode) {
        case MODE_MENU:             motion_menu(a, mx, my, width, height); return;
        case MODE_PREGAME:          motion_pregame(a, mx, my, width, height); return;
        case MODE_CHALLENGE_SELECT: motion_challenge_select(a, mx, my, width, height); return;
        case MODE_OPTIONS:          motion_options(a, mx, my, width, height); return;
        case MODE_CHALLENGE:        motion_challenge(a, mx, my, width, height); return;
        case MODE_PLAYING:          motion_playing(a, mx, my, width, height); return;
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

    // Modal takes priority over every other key handler. ESC on
    // the pieces-missing modal exits to the main menu (matching
    // the only button), since the modal is non-dismissable until
    // the board is fixed; ESC on the withdraw modal just closes it.
    if (a.mode == MODE_PLAYING && a.chessnut_missing_modal_open) {
        if (key == KEY_ESCAPE) app_enter_menu(a);
        return;
    }
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

void app_eval_ready(AppState& a, int cp, int score_index,
                    const std::string& best_uci) {
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

    // Cache the latest bestmove regardless of hint mode — the
    // on-demand voice command ("give me a hint") reads from this
    // cache to surface a hint instantly instead of waiting for a
    // fresh eval round trip. Updated unconditionally so a mode
    // flip from Off → OnDemand mid-turn has the right value
    // ready. Shift the previous one to prev_eval_best_uci so the
    // classifier below has the bestmove for the position the
    // player JUST moved out of (i.e. what they should have
    // played).
    if (!best_uci.empty()) {
        a.prev_eval_best_uci = a.last_eval_best_uci;
        a.last_eval_best_uci = best_uci;
    }

    // Move-quality classification — speak a category ("Best move",
    // "Inaccuracy", "Blunder", ...) right after the eval for the
    // just-played move lands. CP loss is computed from white's
    // perspective by score_history; flip the sign for whichever
    // side just moved. Skipped on the very first eval (no move
    // played yet) and when speak-moves is off.
    if (a.voice_tts_enabled &&
        a.mode == MODE_PLAYING && !gs.game_over &&
        !gs.move_history.empty() && score_index >= 1) {
        // After execute_move, gs.white_turn flipped — so the
        // player who just moved is the opposite colour.
        bool white_just_moved = !gs.white_turn;
        float eval_before = gs.score_history[score_index - 1];
        float eval_after  = gs.score_history[score_index];
        // Convert pawn units → centipawns and flip for player.
        int cp_before = static_cast<int>(eval_before * 100.0f);
        int cp_after  = static_cast<int>(eval_after  * 100.0f);
        int cp_loss = white_just_moved
            ? (cp_before - cp_after)
            : (cp_after - cp_before);
        if (cp_loss < 0) cp_loss = 0;  // can't help yourself by moving

        bool was_best = !a.prev_eval_best_uci.empty() &&
                        gs.move_history.back() == a.prev_eval_best_uci;

        const char* phrase = nullptr;
        if (was_best && cp_loss <= 10)        phrase = "Best move";
        else if (cp_loss <= 15)               phrase = "Excellent move";
        else if (cp_loss <= 50)               phrase = "Good move";
        else if (cp_loss <= 100)              phrase = "Inaccuracy";
        else if (cp_loss <= 200)              phrase = "Mistake";
        else                                  phrase = "Blunder";

        // Suppress the announcement when the move resolves a
        // mate-search overflow (eval_before or eval_after pegged
        // at ±100 pawn-units by app_eval_ready's mate clamp).
        // Those generate noisy classifications because the score
        // delta is dominated by mate-distance changes, not real
        // material loss. Speak only when both endpoints are inside
        // the normal centipawn range.
        bool both_normal = std::abs(eval_before) < 50.0f &&
                           std::abs(eval_after)  < 50.0f;
        if (phrase && both_normal) {
            voice_tts_speak(phrase);
            std::fprintf(stderr,
                "[classify] %s (cp_loss=%d, was_best=%d, "
                "before=%.2f after=%.2f white_moved=%d)\n",
                phrase, cp_loss, was_best ? 1 : 0,
                eval_before, eval_after,
                white_just_moved ? 1 : 0);
        }
    }

    // Decide whether to surface the hint NOW: Auto fires every
    // eval; OnDemand fires only when the voice command set the
    // request flag; Off never fires.
    bool conditions_met =
        !a.two_player_mode &&
        a.mode == MODE_PLAYING &&
        !gs.game_over &&
        !gs.ai_animating &&
        !gs.ai_thinking &&
        gs.white_turn == a.human_plays_white &&
        !best_uci.empty();
    bool surface_now = conditions_met &&
        (a.hint_mode == AppState::HintMode::Auto ||
         (a.hint_mode == AppState::HintMode::OnDemand &&
          a.hint_request_pending));

    if (surface_now) {
        int fc, fr, tc, tr;
        if (parse_uci_move(best_uci, fc, fr, tc, tr)) {
            gs.hint_from_col = fc;
            gs.hint_from_row = fr;
            gs.hint_to_col   = tc;
            gs.hint_to_row   = tr;
            // Speak the hint once per position. ply + uci is a
            // stable key so eval refresh on the same position
            // doesn't repeat. For OnDemand requests the user is
            // explicitly asking, so reset dedup so the next request
            // re-speaks even if the position hasn't changed.
            int ply = static_cast<int>(gs.move_history.size());
            bool was_on_demand_request =
                (a.hint_mode == AppState::HintMode::OnDemand &&
                 a.hint_request_pending);
            if (gs.hint_last_spoken_uci != best_uci ||
                gs.hint_last_spoken_ply != ply) {
                gs.hint_last_spoken_uci = best_uci;
                gs.hint_last_spoken_ply = ply;
                if (!gs.snapshots.empty()) {
                    std::string spoken = uci_to_speech(
                        gs.snapshots.back(), best_uci);
                    // OnDemand path follows the spoken hint with
                    // a "Do you want to play this?" prompt and
                    // arms hint_confirm_pending so yes/no plays
                    // the move or dismisses. Auto path just
                    // announces.
                    if (was_on_demand_request) {
                        voice_tts_speak("Hint: " + spoken +
                            ". Do you want to play this?");
                        a.hint_confirm_pending = true;
                        set_status(a,
                            "Hint shown — say \"yes\" to play it "
                            "or \"no\" to dismiss");
                    } else {
                        voice_tts_speak("Hint: " + spoken);
                    }
                }
            }
            a.hint_request_pending = false;  // consume
        }
    } else if (a.hint_mode != AppState::HintMode::Auto) {
        // OnDemand idle / Off: drop any stale rings. (Auto leaves
        // them so the eval-refresh loop doesn't briefly hide them.)
        gs.hint_from_col = gs.hint_from_row = -1;
        gs.hint_to_col   = gs.hint_to_row   = -1;
    }

    queue_redraw(a);
}

// ===========================================================================
// Per-frame tick
// ===========================================================================
// ===========================================================================
// Per-frame tick
// ===========================================================================
static void tick_menu_physics(AppState& a, int64_t now) {
    if (a.mode != MODE_MENU) return;
    float dt = static_cast<float>(
        static_cast<double>(now - a.menu_last_update_us) / 1e6);
    a.menu_last_update_us = now;
    if (dt < 0.0f)  dt = 0.0f;
    if (dt > 0.05f) dt = 0.05f;
    menu_update_physics(a.menu_pieces, dt);
    queue_redraw(a);
}

// Commit an AI / sensor-driven animation when its duration has
// elapsed: run the move, play SFX, kick the eval refresh in live
// play. Two flags from the sensor handler tweak the tail end:
//   * ai_anim_skip_chessnut_sync — piece is already at its
//     destination on the physical board (set in both 1P and 2P
//     for sensor moves), so just refresh the LED highlight
//     instead of re-driving the firmware.
//   * ai_anim_trigger_ai_after — single-player sensor move just
//     applied, so Stockfish's reply needs to be kicked off.
static void tick_ai_animation(AppState& a, int64_t now) {
    GameState& gs = a.game;
    if (!gs.ai_animating) return;
    float elapsed =
        static_cast<float>(static_cast<double>(now - a.ai_anim_start_us) / 1e6);
    if (elapsed >= gs.ai_anim_duration) {
        gs.ai_animating = false;
        bool sfx_capture = gs.grid[gs.ai_to_row][gs.ai_to_col] >= 0;
        // Capture the pre-move snapshot for TTS rendering before
        // execute_move mutates gs. We only build the SAN / spoken
        // string when the toggle is on, so the BoardSnapshot
        // construction is gated to avoid the cost in the hot path.
        BoardSnapshot tts_before;
        if (a.voice_tts_enabled) {
            tts_before.pieces        = gs.pieces;
            tts_before.white_turn    = gs.white_turn;
            tts_before.castling      = gs.castling;
            tts_before.ep_target_col = gs.ep_target_col;
            tts_before.ep_target_row = gs.ep_target_row;
        }
        execute_move(gs, gs.ai_from_col, gs.ai_from_row,
                     gs.ai_to_col,   gs.ai_to_row);
        gs.hint_from_col = gs.hint_from_row = -1;
        gs.hint_to_col   = gs.hint_to_row   = -1;
        a.hint_confirm_pending = false;
        play_move_sfx(gs, sfx_capture);
        // Speak the AI's reply over the speaker. Skipped when the
        // toggle is off; the user's own moves are never announced
        // (reading them back is redundant noise).
        if (a.voice_tts_enabled && !gs.move_history.empty()) {
            voice_tts_speak(uci_to_speech(tts_before, gs.move_history.back()));
        }
        gs.ai_thinking = false;
        app_refresh_status(a);
        if (gs.ai_anim_skip_chessnut_sync) {
            gs.ai_anim_skip_chessnut_sync = false;
            app_chessnut_highlight_last_move(a);
        } else {
            app_chessnut_sync_board(a, /*force=*/false);
        }
        bool trigger_ai_after = gs.ai_anim_trigger_ai_after;
        gs.ai_anim_trigger_ai_after = false;
        if (a.mode == MODE_PLAYING) {
            trigger_eval(a, static_cast<int>(gs.score_history.size()) - 1);
        }
        if (trigger_ai_after && a.mode == MODE_PLAYING && !gs.game_over &&
            gs.white_turn != a.human_plays_white) {
            trigger_ai(a);
        }
    }
    queue_redraw(a);
}

// Damped sine board shake driven by either a challenge mistake
// (mate_in_N) or anything that's set a generic board_shake_start_us
// (e.g. an invalid sensor-driven hand-move). Settles to 0 after
// MISTAKE_SHAKE_DURATION; while the challenge-mistake reveal is
// pending, keeps redrawing so the Try-Again button surfaces on the
// same frame the sfx finishes.
static void tick_mistake_shake(AppState& a, int64_t now) {
    bool challenge_active =
        a.mode == MODE_CHALLENGE && a.challenge_mistake;
    bool generic_active = a.board_shake_start_us != 0;
    if (!challenge_active && !generic_active) return;

    int64_t start_us = challenge_active
        ? a.challenge_mistake_start_us : a.board_shake_start_us;
    float t = static_cast<float>(
        static_cast<double>(now - start_us) / 1e6);
    if (t < MISTAKE_SHAKE_DURATION) {
        float pi = static_cast<float>(M_PI);
        a.board_shake_x =
            0.25f * std::exp(-5.0f * t) * std::sin(2.0f * pi * 5.0f * t);
        queue_redraw(a);
    } else {
        if (a.board_shake_x != 0.0f) {
            a.board_shake_x = 0.0f;
            queue_redraw(a);
        }
        // Clear the generic trigger so we don't keep ticking
        // forever after the wobble settles. challenge_mistake is
        // cleared elsewhere (Try-Again / next puzzle).
        if (generic_active && !challenge_active)
            a.board_shake_start_us = 0;
    }
    if (challenge_active && !mistake_reveal_ready(a)) queue_redraw(a);
}

// Withdraw-flag cloth verlet physics. Gated on live gameplay (not
// game-over, analysis, or an open modal). The pause-on-modal latch
// stops a huge dt from dumping into the sim when the user reopens.
static void tick_withdraw_flag(AppState& a, int64_t now) {
    GameState& gs = a.game;
    const bool live = a.mode == MODE_PLAYING && !gs.game_over && !gs.analysis_mode &&
                      !a.withdraw_confirm_open && !a.flag.p.empty();
    if (live) {
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
        a.flag_last_update_us = 0;
    }
}

// Chess clock. Tick the side-to-move's budget, add the Fischer
// increment on turn flips, and set game_over on timeout. Same
// pause-on-modal pattern as the flag so a modal doesn't dump a
// huge dt into a clock.
static void tick_clock(AppState& a, int64_t now) {
    GameState& gs = a.game;
    const bool active = a.mode == MODE_PLAYING && a.clock_enabled && !gs.game_over &&
                        !gs.analysis_mode && !a.withdraw_confirm_open;
    if (!active) {
        if (a.clock_enabled && (a.withdraw_confirm_open || gs.game_over ||
                                gs.analysis_mode)) {
            a.clock_last_tick_us = 0;
        }
        return;
    }
    if (a.clock_last_tick_us == 0) {
        a.clock_last_tick_us = now;
        a.prev_white_turn = gs.white_turn ? 1 : 0;
        queue_redraw(a);
        return;
    }

    int64_t dt_us = now - a.clock_last_tick_us;
    a.clock_last_tick_us = now;
    int cur = gs.white_turn ? 1 : 0;
    // Charge this interval to whoever WAS on move. If the turn
    // flipped between ticks, the move happened mid-interval — the
    // dt belongs to the previous side (they thought, then moved),
    // not the new side.
    if (dt_us > 0) {
        int64_t dt_ms = dt_us / 1000;
        if (a.prev_white_turn == 1) a.white_ms_left -= dt_ms;
        else                        a.black_ms_left -= dt_ms;
    }
    // On turn flip, the side that just moved gets the Fischer
    // increment. Detected here (not inside execute_move) so the
    // rules layer stays pure. Each clock is capped at the initial
    // budget so the display never grows above the starting value
    // (raw Fischer + our adaptive AI time produced a visible "clock
    // counting up" artefact in classical).
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
    queue_redraw(a);
}

// Auto-reconnect tick — desktop body defined in the chessnut block
// below. Web is single-process / single-call (the browser handles
// reconnection inside navigator.bluetooth), so it stubs out here.
#ifndef __EMSCRIPTEN__
static void chessnut_tick_reconnect(AppState& a, int64_t now);
#else
static inline void chessnut_tick_reconnect(AppState&, int64_t) {}
#endif

void app_tick(AppState& a) {
    int64_t now = now_us(a);
    // Safe in every mode; no-op when no track is playing.
    audio_music_tick();

    tick_menu_physics(a, now);
    tick_ai_animation(a, now);

    // Selection ring pulse + shatter transition only need redraws
    // issued; the pulse reads gs.anim_start_time directly and
    // shatter has its own elapsed check in the render path.
    if (a.game.selected_col >= 0) queue_redraw(a);
    if (a.transition_active)      queue_redraw(a);

    tick_mistake_shake(a, now);
    tick_withdraw_flag(a, now);
    tick_clock(a, now);

    // Auto-reconnect to the Chessnut Move while the toggle is on.
    // Helper is declared in the desktop section so it can see the
    // file-static g_chessnut_bridge handle.
    chessnut_tick_reconnect(a, now);
}

// ===========================================================================
// Rendering dispatch
// ===========================================================================
// ===========================================================================
// Rendering dispatch
// ===========================================================================
static void render_menu(AppState& a, int width, int height, int64_t now) {
    float t = static_cast<float>(
        static_cast<double>(now - a.menu_start_time_us) / 1e6);
    static bool s_last_logged_connected = false;
    static bool s_logged_once = false;
    if (!s_logged_once || s_last_logged_connected != a.chessnut_connected) {
        std::fprintf(stderr,
            "[chessnut/menu] render: chessnut_connected=%d "
            "enabled=%d picker_open=%d\n",
            a.chessnut_connected ? 1 : 0,
            a.chessnut_enabled   ? 1 : 0,
            a.chessnut_picker_open ? 1 : 0);
        s_logged_once = true;
        s_last_logged_connected = a.chessnut_connected;
    }
    renderer_draw_menu(a.menu_pieces, width, height, t, a.menu_hover,
                       /*cartoon_outline=*/a.cartoon_outline,
                       /*chessnut_connected=*/a.chessnut_connected);
}

static void render_pregame(AppState& a, int width, int height) {
    renderer_draw_pregame(a.human_plays_white, a.stockfish_elo,
                          APP_ELO_MIN, APP_ELO_MAX,
                          a.time_control,
                          a.pregame_tc_open,
                          a.pregame_tc_hover,
                          /*hide_elo_slider=*/a.two_player_mode,
                          width, height, a.pregame_hover);
}

static void render_challenge_select(AppState& a, int width, int height) {
    renderer_draw_challenge_select(
        a.challenge_names, width, height, a.challenge_select_hover);
}

static void render_options(AppState& a, int width, int height) {
    bool voice_supported = app_voice_continuous_supported();
    bool chessnut_supported = app_chessnut_supported();
    // Project AppState's std::string-backed device list into the
    // C-pointer struct the renderer expects. Lifetime is fine —
    // the strings outlive this stack frame and the renderer
    // doesn't retain pointers.
    std::vector<OptionsScannedDevice> devs;
    devs.reserve(a.chessnut_devices.size());
    for (const auto& d : a.chessnut_devices) {
        devs.push_back({d.address.c_str(), d.name.c_str()});
    }
    renderer_draw_options(a.cartoon_outline,
                          voice_supported && a.voice_continuous_enabled,
                          voice_supported,
                          voice_supported && a.voice_tts_enabled,
                          static_cast<int>(a.hint_mode),
                          chessnut_supported && a.chessnut_enabled,
                          chessnut_supported,
                          a.ble_verbose_log,
                          a.chessnut_picker_open,
                          a.chessnut_picker_scanning,
                          devs.data(),
                          static_cast<int>(devs.size()),
                          width, height, a.options_hover);
}

static void render_challenge_summary(AppState& a, int width, int height) {
    std::vector<SummaryEntry> entries;
    entries.reserve(a.challenge_solutions.size());
    for (size_t i = 0; i < a.challenge_solutions.size(); i++) {
        SummaryEntry e;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "Puzzle %zu", i + 1);
        e.puzzle_name = buf;
        e.moves = a.challenge_solutions[i];
        entries.push_back(std::move(e));
    }
    renderer_draw_challenge_summary(
        a.current_challenge.name, entries, width, height);
}

// Draw the 3D board + HUD used by both live play and challenge mode.
// During challenge mode the desktop game_over overlay is suppressed
// (the Next button replaces it); we save/restore the GameState's
// game_over fields around the call to keep the rules layer pure.
static void render_board(AppState& a, int width, int height) {
    GameState& gs = a.game;
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
    // cells) so we don't guard against extra re-inits.
    if (a.mode == MODE_PLAYING &&
        (a.flag.inited_w != width || a.flag.inited_h != height)) {
        flag_init(a.flag, width, height);
        a.flag_last_update_us = 0;
    }

    const bool draw_flag =
        a.mode == MODE_PLAYING &&
        !gs.game_over && !gs.analysis_mode && !a.withdraw_confirm_open;

    // Clock widget: same visibility gate as the flag, plus the time
    // control must be non-Unlimited. The side shown is whoever is
    // on move.
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

    // Pieces-missing modal sits on top of everything else in the
    // playing scene. Drawn after renderer_draw so it overlays the
    // game-over / withdraw modals if they happen to coincide.
    if (a.chessnut_missing_modal_open) {
        ChessnutBoardModalKind kind = ChessnutBoardModalKind::Missing;
        switch (a.chessnut_missing_modal_type) {
        case AppState::ChessnutModalType::Positioning:
            kind = ChessnutBoardModalKind::Positioning; break;
        case AppState::ChessnutModalType::Missing:
            kind = ChessnutBoardModalKind::Missing;     break;
        case AppState::ChessnutModalType::WrongLayout:
            kind = ChessnutBoardModalKind::WrongLayout; break;
        }
        renderer_draw_chessnut_missing_modal(
            a.chessnut_missing_squares_msg,
            kind,
            a.chessnut_missing_exit_hover);
    }

    if (a.mode == MODE_CHALLENGE) {
        gs.game_over   = save_game_over;
        gs.game_result = save_result;
    }
}

static void render_challenge_overlay_and_buttons(AppState& a, int width, int height) {
    const std::string& ct = a.current_challenge.type;
    bool is_tactic = is_tactic_type(ct);
    std::string tactic_label = is_tactic
        ? (ct == "find_forks" ? "Forks" : "Pins") : "";
    int tactic_required = is_tactic
        ? static_cast<int>(a.current_challenge.required_moves.size()) : 0;
    int tactic_found = is_tactic
        ? static_cast<int>(a.current_challenge.found_moves.size()) : 0;
    renderer_draw_challenge_overlay(
        a.current_challenge.name,
        a.current_challenge.current_index,
        static_cast<int>(a.current_challenge.fens.size()),
        a.challenge_moves_made,
        a.current_challenge.max_moves,
        a.current_challenge.starts_white,
        tactic_label, tactic_found, tactic_required,
        width, height);

    if (a.challenge_solved && !a.transition_active &&
        a.transition_pending_next < 0) {
        renderer_draw_next_button(width, height, a.challenge_next_hover);
    }
    // Try Again only appears after the shake has settled AND the
    // mistake sfx has played out (see mistake_reveal_ready), so the
    // mistake feedback has a clear "then" beat.
    if (mistake_reveal_ready(a)) {
        renderer_draw_try_again_button(width, height, a.challenge_try_again_hover);
    }
}

// Run the glass-shatter puzzle transition: capture the current
// frame, load the next puzzle, redraw the new state, then return so
// the main render path lets the shatter overlay animate the captured
// texture fading away.
static void render_challenge_transition_trigger(AppState& a, int width, int height,
                                                int64_t now) {
    if (a.transition_pending_next < 0) return;
    renderer_capture_frame(width, height);
    app_load_challenge_puzzle(a, a.transition_pending_next);
    a.transition_pending_next = -1;
    a.transition_active = true;
    a.transition_start_time_us = now;
    audio_play(SoundEffect::GlassBreak);

    // Challenge mode never wants the withdraw flag, modal, or clock.
    // Reuse the session cartoon_outline so toggling survives the
    // transition.
    renderer_draw(a.game, width, height, a.rot_x, a.rot_y, a.zoom,
                  a.human_plays_white,
                  a.endgame_menu_hover, false,
                  nullptr, false, false, 0,
                  false, 0, false,
                  a.cartoon_outline);
    {
        const std::string& ct = a.current_challenge.type;
        bool is_tactic = is_tactic_type(ct);
        std::string tactic_label = is_tactic
            ? (ct == "find_forks" ? "Forks" : "Pins") : "";
        int tactic_required = is_tactic
            ? static_cast<int>(std::min<size_t>(
                3, a.current_challenge.required_moves.size())) : 0;
        int tactic_found = is_tactic
            ? static_cast<int>(a.current_challenge.found_moves.size()) : 0;
        renderer_draw_challenge_overlay(
            a.current_challenge.name,
            a.current_challenge.current_index,
            static_cast<int>(a.current_challenge.fens.size()),
            a.challenge_moves_made,
            a.current_challenge.max_moves,
            a.current_challenge.starts_white,
            tactic_label, tactic_found, tactic_required,
            width, height);
    }
}

static void render_challenge_transition_overlay(AppState& a, int width, int height,
                                                int64_t now) {
    if (!a.transition_active) return;
    float t = static_cast<float>(
        static_cast<double>(now - a.transition_start_time_us) / 1e6);
    constexpr float TRANSITION_DURATION = 1.3f;
    if (t >= TRANSITION_DURATION) {
        a.transition_active = false;
    } else {
        renderer_draw_shatter(t, width, height);
    }
}

void app_render(AppState& a, int width, int height) {
    int64_t now = now_us(a);

    if (a.mode == MODE_MENU)             { render_menu(a, width, height, now); return; }
    if (a.mode == MODE_PREGAME)          { render_pregame(a, width, height);   return; }
    if (a.mode == MODE_CHALLENGE_SELECT) { render_challenge_select(a, width, height); return; }
    if (a.mode == MODE_OPTIONS)          { render_options(a, width, height);   return; }
    if (a.mode == MODE_CHALLENGE && a.challenge_show_summary) {
        render_challenge_summary(a, width, height);
        return;
    }

    // Live game + in-progress challenge share the 3D board path,
    // then challenge layers its overlay + transition on top.
    render_board(a, width, height);
    if (a.mode != MODE_CHALLENGE) return;
    render_challenge_overlay_and_buttons(a, width, height);
    render_challenge_transition_trigger(a, width, height, now);
    render_challenge_transition_overlay(a, width, height, now);
}

// ===========================================================================
// Voice input — universal helpers (used by both desktop whisper.cpp and
// web SpeechRecognition drivers).
// ===========================================================================
namespace {

// Returns true when voice can run right now (right mode, our turn,
// nothing blocking). Doesn't touch any state — pure check.
bool voice_action_allowed(const AppState& a) {
    if (a.mode != MODE_PLAYING) return false;
    const GameState& gs = a.game;
    if (gs.ai_thinking || gs.ai_animating) return false;
    if (gs.analysis_mode || gs.game_over)  return false;
    if (gs.white_turn != a.human_plays_white) return false;
    return true;
}

// Try the supplied utterance against the UI-button command parser and
// dispatch if it matches. Returns true if a command fired (caller
// should not also try parsing as a chess move).
bool try_voice_command(AppState& a, const std::string& utterance) {
    VoiceCommandContext ctx;
    ctx.mode                     = a.mode;
    ctx.game_over                = a.game.game_over;
    ctx.analysis_mode            = a.game.analysis_mode;
    ctx.challenge_solved         = a.challenge_solved;
    ctx.challenge_mistake_ready  = mistake_reveal_ready(a);
    ctx.challenge_show_summary   = a.challenge_show_summary;
    ctx.withdraw_confirm_open    = a.withdraw_confirm_open;
    ctx.hint_confirm_open        = a.hint_confirm_pending;

    VoiceCommand cmd = parse_voice_command(utterance, ctx);
    if (cmd == VoiceCommand::None) return false;

    switch (cmd) {
    case VoiceCommand::BackToMenu:
        app_enter_menu(a);
        break;
    case VoiceCommand::StartGame:
        if (a.mode == MODE_MENU)         app_enter_pregame(a);
        else if (a.mode == MODE_PREGAME) app_enter_game(a);
        break;
    case VoiceCommand::NewGame:
        app_enter_pregame(a);
        break;
    case VoiceCommand::OpenOptions:
        app_enter_options(a);
        break;
    case VoiceCommand::OpenChallenges:
        app_enter_challenge_select(a);
        break;
    case VoiceCommand::ContinuePlaying:
        if (a.game.analysis_mode) {
            game_exit_analysis(a.game);
            a.continue_playing_hover = false;
            a.endgame_menu_hover = false;
            app_refresh_status(a);
        }
        break;
    case VoiceCommand::Resign:
        // Same effect as clicking the withdraw flag: open the
        // confirmation modal. The user confirms by saying yes/no
        // (handled below) or by clicking the modal buttons.
        if (a.mode == MODE_PLAYING && !a.game.game_over &&
            !a.game.analysis_mode) {
            a.withdraw_confirm_open = true;
            a.withdraw_hover = 0;
        }
        break;
    case VoiceCommand::ConfirmYes:
        if (a.withdraw_confirm_open) {
            a.withdraw_confirm_open = false;
            a.withdraw_hover = 0;
            app_enter_menu(a);
        } else if (a.hint_confirm_pending) {
            // User accepted the hint — play it. Mirror the voice
            // move-execution path (capture detection, sfx, TTS,
            // chessnut sync, eval kick, AI trigger).
            a.hint_confirm_pending = false;
            const std::string uci = a.last_eval_best_uci;
            int fc, fr, tc, tr;
            if (uci.empty() || !parse_uci_move(uci, fc, fr, tc, tr)) {
                set_status(a, "Hint move expired — try again");
                queue_redraw(a);
                break;
            }
            // Sanity: the hint must still match the user's turn /
            // single-player constraints. If anything's drifted
            // (the AI may have already moved, etc.), refuse rather
            // than play the wrong side's move.
            if (a.two_player_mode || a.mode != MODE_PLAYING ||
                a.game.game_over || a.game.ai_animating ||
                a.game.ai_thinking ||
                a.game.white_turn != a.human_plays_white) {
                set_status(a, "Hint move expired — try again");
                a.game.hint_from_col = a.game.hint_from_row = -1;
                a.game.hint_to_col   = a.game.hint_to_row   = -1;
                queue_redraw(a);
                break;
            }
            GameState& gs = a.game;
            bool sfx_capture = gs.grid[tr][tc] >= 0;
            BoardSnapshot tts_before;
            if (a.voice_tts_enabled) {
                tts_before.pieces        = gs.pieces;
                tts_before.white_turn    = gs.white_turn;
                tts_before.castling      = gs.castling;
                tts_before.ep_target_col = gs.ep_target_col;
                tts_before.ep_target_row = gs.ep_target_row;
            }
            execute_move(gs, fc, fr, tc, tr);
            gs.selected_col = gs.selected_row = -1;
            gs.valid_moves.clear();
            gs.hint_from_col = gs.hint_from_row = -1;
            gs.hint_to_col   = gs.hint_to_row   = -1;
            play_move_sfx(gs, sfx_capture);
            if (a.voice_tts_enabled && !gs.move_history.empty()) {
                voice_tts_speak(uci_to_speech(
                    tts_before, gs.move_history.back()));
            }
            app_chessnut_sync_board(a, /*force=*/false);
            app_refresh_status(a);
            trigger_eval(a, static_cast<int>(gs.score_history.size()) - 1);
            if (!a.two_player_mode &&
                gs.white_turn != a.human_plays_white && !gs.game_over)
                trigger_ai(a);
            queue_redraw(a);
        }
        break;
    case VoiceCommand::ConfirmNo:
        if (a.withdraw_confirm_open) {
            a.withdraw_confirm_open = false;
            a.withdraw_hover = 0;
        } else if (a.hint_confirm_pending) {
            // User declined — wipe the hint rings and the prompt.
            a.hint_confirm_pending = false;
            a.game.hint_from_col = a.game.hint_from_row = -1;
            a.game.hint_to_col   = a.game.hint_to_row   = -1;
            a.game.hint_last_spoken_uci.clear();
            a.game.hint_last_spoken_ply = -1;
            set_status(a, "Hint dismissed");
            queue_redraw(a);
        }
        break;
    case VoiceCommand::NextPuzzle: {
        int next = a.current_challenge.current_index + 1;
        if (next < static_cast<int>(a.current_challenge.fens.size())) {
            a.transition_pending_next = next;
        } else {
            a.challenge_show_summary = true;
        }
        break;
    }
    case VoiceCommand::TryAgain:
        app_reset_challenge_puzzle(a);
        break;
    case VoiceCommand::ToggleCartoonOutline:
        a.cartoon_outline = !a.cartoon_outline;
        break;
    case VoiceCommand::ToggleContinuousVoice:
        app_voice_toggle_continuous_request(a);
        break;
    case VoiceCommand::ToggleChessnut:
        if (app_chessnut_supported()) app_chessnut_toggle_request(a);
        break;
    case VoiceCommand::ToggleBleVerbose:
        a.ble_verbose_log = !a.ble_verbose_log;
        set_status(a, a.ble_verbose_log
            ? "BLE verbose log: ON — every notify frame will surface here"
            : "BLE verbose log: OFF");
        break;
    case VoiceCommand::ToggleSpeakMoves:
        if (app_voice_continuous_supported())
            app_voice_toggle_speak_moves_request(a);
        break;
    case VoiceCommand::ToggleHints:
        // Cycle Off → Auto → OnDemand → Off, same as the Options
        // click handler. Spoken phrases only fire in MODE_OPTIONS
        // so the parser already gates this.
        switch (a.hint_mode) {
        case AppState::HintMode::Off:
            a.hint_mode = AppState::HintMode::Auto;
            set_status(a, "Move hints: AUTO");
            break;
        case AppState::HintMode::Auto:
            a.hint_mode = AppState::HintMode::OnDemand;
            set_status(a, "Move hints: ON DEMAND");
            break;
        case AppState::HintMode::OnDemand:
            a.hint_mode = AppState::HintMode::Off;
            set_status(a, "Move hints: OFF");
            break;
        }
        a.game.hint_from_col = a.game.hint_from_row = -1;
        a.game.hint_to_col   = a.game.hint_to_row   = -1;
        a.game.hint_last_spoken_uci.clear();
        a.game.hint_last_spoken_ply = -1;
        a.hint_request_pending = false;
        a.hint_confirm_pending = false;
        break;
    case VoiceCommand::RequestHint: {
        // One-shot hint request from voice. Three sub-cases:
        //   * Off: gentle nudge; user has to opt in via Options
        //          first. We don't auto-flip the mode because
        //          that's a meta-action they didn't ask for.
        //   * Auto: the hint is already showing (or will on the
        //          next eval). Re-speak the cached one for
        //          symmetry — same UX as asking "what is it
        //          again?".
        //   * OnDemand: the meat. Surface the cached bestmove
        //          immediately if we have one; otherwise mark
        //          the request pending so the next eval
        //          surfaces it.
        if (a.hint_mode == AppState::HintMode::Off) {
            set_status(a,
                "Move hints are off — say \"move hints\" or "
                "click the Options row to enable");
            break;
        }
        // Reset dedup so a repeated request re-speaks the same
        // hint — the user is explicitly asking again.
        a.game.hint_last_spoken_uci.clear();
        a.game.hint_last_spoken_ply = -1;
        if (!a.last_eval_best_uci.empty() &&
            !a.two_player_mode &&
            a.mode == MODE_PLAYING &&
            !a.game.game_over &&
            !a.game.ai_animating &&
            !a.game.ai_thinking &&
            a.game.white_turn == a.human_plays_white) {
            int fc, fr, tc, tr;
            if (parse_uci_move(a.last_eval_best_uci, fc, fr, tc, tr)) {
                a.game.hint_from_col = fc;
                a.game.hint_from_row = fr;
                a.game.hint_to_col   = tc;
                a.game.hint_to_row   = tr;
                if (!a.game.snapshots.empty()) {
                    // Speak the move + ask for confirmation in
                    // one utterance so the prompt arrives on the
                    // same voice channel without the user
                    // wondering whether the hint is finished. The
                    // dispatcher routes the next yes/no through
                    // the hint_confirm_pending branch above.
                    std::string move_speech = uci_to_speech(
                        a.game.snapshots.back(),
                        a.last_eval_best_uci);
                    voice_tts_speak("Hint: " + move_speech +
                                    ". Do you want to play this?");
                }
                a.hint_confirm_pending = true;
                set_status(a,
                    "Hint shown — say \"yes\" to play it or \"no\" "
                    "to dismiss");
                queue_redraw(a);
            }
        } else {
            // No cached bestmove yet — let the next eval landing
            // surface the hint when it arrives.
            a.hint_request_pending = true;
            set_status(a, "Hint requested — computing…");
        }
        break;
    }
    case VoiceCommand::PlayWhite:
        a.human_plays_white = true;
        break;
    case VoiceCommand::PlayBlack:
        a.human_plays_white = false;
        break;
    case VoiceCommand::None:
        return false;
    }

    std::string msg = std::string("Voice — \"") + utterance + "\"";
    set_status(a, msg.c_str());
    queue_redraw(a);
    return true;
}

// Shared body for both push-to-talk (app_voice_apply_result) and
// continuous (app_voice_continuous_apply) result delivery. Caller is
// responsible for any flag bookkeeping (e.g. clearing voice_listening).
void apply_voice_utterance(AppState& a,
                           const std::string& utterance,
                           const std::string& error) {
    if (!error.empty()) {
        std::string msg = "Voice: " + error;
        set_status(a, msg.c_str());
        queue_redraw(a);
        return;
    }

    // UI-button commands first — they're mode-specific so a chess
    // move utterance like "knight d3" never accidentally triggers
    // one. Falls through to move parsing if nothing matches.
    if (try_voice_command(a, utterance)) return;

    // The withdraw confirmation modal is, well, modal. If it's open
    // and we got here, the utterance wasn't yes/no — surface a hint
    // and don't try to parse it as a chess move.
    if (a.withdraw_confirm_open) {
        std::string msg = std::string("Voice — say 'yes' or 'no' (heard '") +
                          utterance + "')";
        set_status(a, msg.c_str());
        queue_redraw(a);
        return;
    }

    GameState& gs = a.game;
    if (!voice_action_allowed(a)) {
        std::string msg = std::string("Voice ignored '") + utterance +
                          "' — not your turn";
        set_status(a, msg.c_str());
        queue_redraw(a);
        return;
    }

    std::string uci, perr;
    if (!parse_voice_move(utterance, gs, uci, perr)) {
        std::string msg = "Voice — heard '" + utterance + "': " + perr;
        set_status(a, msg.c_str());
        queue_redraw(a);
        return;
    }

    int from_col, from_row, to_col, to_row;
    if (!parse_uci_move(uci, from_col, from_row, to_col, to_row)) {
        set_status(a, "Voice — internal error decoding parser UCI");
        queue_redraw(a);
        return;
    }

    bool sfx_capture = gs.grid[to_row][to_col] >= 0;
    BoardSnapshot tts_before;
    if (a.voice_tts_enabled) {
        tts_before.pieces        = gs.pieces;
        tts_before.white_turn    = gs.white_turn;
        tts_before.castling      = gs.castling;
        tts_before.ep_target_col = gs.ep_target_col;
        tts_before.ep_target_row = gs.ep_target_row;
    }
    execute_move(gs, from_col, from_row, to_col, to_row);
    gs.selected_col = gs.selected_row = -1;
    gs.valid_moves.clear();
    gs.hint_from_col = gs.hint_from_row = -1;
    gs.hint_to_col   = gs.hint_to_row   = -1;
    a.hint_confirm_pending = false;
    play_move_sfx(gs, sfx_capture);
    if (a.voice_tts_enabled && !gs.move_history.empty()) {
        voice_tts_speak(uci_to_speech(tts_before, gs.move_history.back()));
    }
    app_chessnut_sync_board(a, /*force=*/false);

    std::string msg = std::string("Voice — heard '") + utterance +
                      "' (" + uci + ")";
    set_status(a, msg.c_str());
    queue_redraw(a);

    // Same post-move flow as a successful click in MODE_PLAYING:
    // refresh status, kick the eval, and dispatch the AI's reply.
    app_refresh_status(a);
    trigger_eval(a, static_cast<int>(gs.score_history.size()) - 1);
    if (!a.two_player_mode &&
        gs.white_turn != a.human_plays_white && !gs.game_over)
        trigger_ai(a);
}

}  // namespace

void app_voice_continuous_apply(AppState& a,
                                const std::string& utterance,
                                const std::string& error) {
    // Drop late deliveries from a worker that finished after the
    // toggle was turned off — apply_voice_utterance would otherwise
    // happily play the move.
    if (!a.voice_continuous_enabled) return;
    apply_voice_utterance(a, utterance, error);
}

void app_voice_continuous_apply_partial(AppState& a,
                                        const std::string& partial) {
    if (!a.voice_continuous_enabled) return;
    if (partial.empty()) return;
    std::string msg = "Hearing: '" + partial + "'";
    set_status(a, msg.c_str());
}

// "Speak moves" toggle. Lazy-initialises the TTS engine on first
// enable; idempotent. Same shape as continuous-voice toggle but
// simpler because there's no async input to plumb — TTS is one-way.
void app_voice_toggle_speak_moves_request(AppState& a) {
    bool target = !a.voice_tts_enabled;
    if (target) {
        std::string err;
        if (!voice_tts_init(err)) {
            std::string msg = "Speak moves unavailable: " + err;
            set_status(a, msg.c_str());
            queue_redraw(a);
            return;
        }
        a.voice_tts_enabled = true;
        set_status(a, "Speak moves: ON — opponent moves will be announced");
    } else {
        a.voice_tts_enabled = false;
        set_status(a, "Speak moves: OFF");
    }
    queue_redraw(a);
}

// ===========================================================================
// Voice push-to-talk (SPACE) + desktop continuous mode — whisper.cpp,
// desktop only. Web has its own continuous-mode driver in
// web/voice_web.cpp.
// ===========================================================================
#ifndef __EMSCRIPTEN__

namespace {

// Resolve the model path at first-use time so users can override via
// CHESS_WHISPER_MODEL without recompiling.
std::string voice_model_path() {
    if (const char* env = std::getenv("CHESS_WHISPER_MODEL"))
        if (*env) return env;
    return "third_party/whisper-models/ggml-distil-small.en.bin";
}

}  // namespace

void app_voice_press(AppState& a) {
    if (a.voice_continuous_enabled) {
        set_status(a,
            "Continuous mode is on — toggle off in Options to use SPACE");
        return;
    }
    if (!voice_action_allowed(a)) return;
    if (a.voice_listening) {
        set_status(a, "Voice — still transcribing previous utterance");
        return;
    }
    if (!a.voice_initialized) {
        if (a.voice_init_failed) return;  // sticky — already reported
        std::string err;
        set_status(a, "Voice — loading model...");
        if (!voice_init(voice_model_path(), err)) {
            a.voice_init_failed = true;
            std::string msg = "Voice unavailable: " + err +
                              " (run 'make fetch-whisper-model')";
            set_status(a, msg.c_str());
            return;
        }
        a.voice_initialized = true;
    }
    voice_start_capture();
    a.voice_listening = true;
    set_status(a, "Voice — listening (release SPACE to send)");
    queue_redraw(a);
}

void app_voice_release(
    AppState& a,
    std::function<void(const std::string& utterance,
                       const std::string& error)> on_done) {
    if (a.voice_continuous_enabled) return;  // SPACE is suppressed
    if (!a.voice_listening) return;
    // Capture-stop / worker-dispatch happens in voice_stop_and_transcribe.
    // The flag stays set until the GUI-thread tail clears it.
    voice_stop_and_transcribe(std::move(on_done));
    set_status(a, "Voice — transcribing...");
    queue_redraw(a);
}

void app_voice_apply_result(AppState& a,
                            const std::string& utterance,
                            const std::string& error) {
    a.voice_listening = false;
    apply_voice_utterance(a, utterance, error);
}

void app_voice_set_continuous(
    AppState& a, bool on,
    std::function<void(const std::string& utterance,
                       const std::string& error)> on_utterance,
    std::function<void(const std::string& partial)> on_partial) {
    if (on == a.voice_continuous_enabled) return;  // idempotent

    if (!on) {
        voice_stop_continuous();
        a.voice_continuous_enabled = false;
        set_status(a, "Voice — continuous listening off");
        queue_redraw(a);
        return;
    }

    // Turning on. Lazy-init if needed; sticky failure otherwise.
    if (!a.voice_initialized) {
        if (a.voice_init_failed) {
            set_status(a, "Voice unavailable — see README");
            queue_redraw(a);
            return;
        }
        std::string err;
        set_status(a, "Voice — loading model...");
        if (!voice_init(voice_model_path(), err)) {
            a.voice_init_failed = true;
            std::string msg = "Voice unavailable: " + err +
                              " (run 'make fetch-whisper-model')";
            set_status(a, msg.c_str());
            queue_redraw(a);
            return;
        }
        a.voice_initialized = true;
    }

    std::string err;
    if (!voice_start_continuous(std::move(on_utterance),
                                std::move(on_partial), err)) {
        std::string msg = "Voice continuous start failed: " + err;
        set_status(a, msg.c_str());
        queue_redraw(a);
        return;
    }
    a.voice_continuous_enabled = true;
    // Auto-enable TTS so a single click on "Continuous voice" gives
    // the full eyes-free voice experience (mic in + speech out).
    // The user can still flip TTS off independently via the
    // "Speak moves" toggle if they only want the mic on.
    if (!a.voice_tts_enabled) {
        std::string tts_err;
        if (voice_tts_init(tts_err)) a.voice_tts_enabled = true;
    }
    set_status(a, "Voice — continuous listening on");
    queue_redraw(a);
}

void app_voice_shutdown(AppState& a) {
    if (a.voice_initialized) {
        voice_shutdown();
        a.voice_initialized = false;
    }
    a.voice_listening = false;
    a.voice_continuous_enabled = false;
}

#endif  // !__EMSCRIPTEN__

// ===========================================================================
// Chessnut Move physical-board mirroring
// ===========================================================================
//
// app_chessnut_apply_status is platform-independent — it interprets
// the status protocol (READY / CONNECTED / DISCONNECTED / ERROR /
// ACK / NOTIFY) emitted by both the desktop bridge and the web
// driver, and updates AppState + status bar accordingly.
//
// app_chessnut_set_enabled / sync_board / shutdown / toggle_request /
// supported are platform-specific:
//   - Desktop: defined just below, drives ChessnutBridge.
//   - Web:     defined in web/chessnut_web.cpp, drives
//              navigator.bluetooth via EM_JS shims.
//
// app_current_fen is exposed on AppState so the web side can
// snapshot the position without re-implementing FEN serialisation.

std::string app_current_fen(const AppState& a) {
    return current_fen(a.game, a.game.white_turn);
}

// ---------------------------------------------------------------------------
// Inbound sensor-frame mirroring (board → digital).
// ---------------------------------------------------------------------------
namespace {

bool parse_hex_byte(char hi, char lo, uint8_t& out) {
    auto v = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    int h = v(hi), l = v(lo);
    if (h < 0 || l < 0) return false;
    out = static_cast<uint8_t>((h << 4) | l);
    return true;
}

// Build a [row][col] grid char snapshot of the digital game's
// piece placement, matching the convention chessnut_encode.h uses
// for the inbound decoder.
std::array<std::array<char, 8>, 8>
digital_grid_snapshot(const GameState& gs) {
    std::array<std::array<char, 8>, 8> grid{};
    for (auto& row : grid) row.fill(' ');
    for (const auto& p : gs.pieces) {
        if (!p.alive) continue;
        if (p.row < 0 || p.row > 7 || p.col < 0 || p.col > 7) continue;
        grid[p.row][p.col] = piece_to_fen(p.type, p.is_white);
    }
    return grid;
}

// True for everything we should refuse to act on automatically:
// AI thinking / animating, modal dialogs open, game already over,
// challenge already solved, etc. The user can still drive the
// physical board through these states; we just don't promote the
// physical move into the digital game.
bool sensor_action_allowed(const AppState& a) {
    if (a.game.ai_thinking || a.game.ai_animating) return false;
    if (a.withdraw_confirm_open) return false;
    if (a.chessnut_missing_modal_open) return false;
    if (a.game.game_over) return false;
    if (a.game.analysis_mode) return false;
    if (a.mode != MODE_PLAYING && a.mode != MODE_CHALLENGE) return false;
    if (a.mode == MODE_CHALLENGE &&
        (a.challenge_solved || a.challenge_mistake)) return false;
    return true;
}

}  // namespace

void app_chessnut_apply_sensor_frame(AppState& a,
                                     const std::string& hex) {
    // Sensor frame layout (per ChessnutService.java:1011-1015 and
    // formatFen at :742): [OPCODE_FEN_DATA, FEN_DATA_PAYLOAD_LEN,
    // <32 board bytes>, <4-byte trailer>] — see chessnut_encode.h
    // for the constants. Reading the board from offset 0 (as we
    // did pre-fix) misaligns by 2 bytes and scrambles every
    // square, surfacing as ~31 bogus diffs on a single-piece move.
    if (hex.size() < chessnut::FEN_DATA_HEX_CHARS) {
        std::fprintf(stderr,
            "[chessnut/sensor] frame too short (%zu hex chars)\n",
            hex.size());
        return;
    }
    uint8_t hdr0 = 0, hdr1 = 0;
    if (!parse_hex_byte(hex[0], hex[1], hdr0) ||
        !parse_hex_byte(hex[2], hex[3], hdr1)) {
        std::fprintf(stderr,
            "[chessnut/sensor] non-hex header bytes\n");
        return;
    }
    if (hdr0 != chessnut::OPCODE_FEN_DATA ||
        hdr1 != chessnut::FEN_DATA_PAYLOAD_LEN) {
        // Not a board-state push (probably an OPCODE_INFO_REPLY for
        // getMovePieceState, an ack, or a power-level frame). Ignore.
        std::fprintf(stderr,
            "[chessnut/sensor] non-board frame opcode=0x%02x len=0x%02x — ignoring\n",
            hdr0, hdr1);
        return;
    }
    std::array<uint8_t, 32> bytes{};
    // Skip the 2-byte header — board nibbles start at hex[4..67].
    for (int i = 0; i < 32; i++) {
        if (!parse_hex_byte(hex[4 + i * 2], hex[4 + i * 2 + 1], bytes[i])) {
            std::fprintf(stderr,
                "[chessnut/sensor] non-hex char at byte offset %d\n", i + 2);
            return;
        }
    }
    auto sensor  = chessnut::board_bytes_to_grid(bytes);
    auto digital = digital_grid_snapshot(a.game);

    // Settling window — motors are mid-motion, sensor frames during
    // this period are transient and don't match the eventual
    // resting state. Used both to silence move-detection during the
    // window and to suppress error chatter from our own animations.
    // 4 s covers the blink-before-motor (~1 s) plus the motor's
    // glide (~1-2 s) with a safety margin; if this is too short,
    // mid-motor transit frames leak out and can look like illegal
    // hand-moves.
    constexpr int64_t kSyncSettlingUs = 4'000'000;  // 4 s
    int64_t now = now_us(a);
    bool settling = (a.chessnut_last_sync_us != 0) &&
                    (now - a.chessnut_last_sync_us < kSyncSettlingUs);

    auto& prev = a.chessnut_last_sensor_grid;
    // Always copy the new sensor reading into prev at end-of-frame
    // — the *next* frame should diff against this one. The
    // baseline_set flag is updated explicitly in the dedicated
    // "establish baseline" branch below, not here, so we can hold
    // off until motors finish settling before declaring a baseline.
    struct PrevUpdater {
        std::array<std::array<char, 8>, 8>& target;
        const std::array<std::array<char, 8>, 8>& source;
        ~PrevUpdater() { target = source; }
    } prev_guard{prev, sensor};

    // Compute deltas against the *previous* sensor frame (the move
    // the user actually made on the board) AND the static
    // disagreements with the digital state (purely diagnostic).
    struct Diff { int row, col; char before, after; };
    std::vector<Diff> deltas;        // sensor[t-1] vs sensor[t]
    std::vector<Diff> digital_diffs; // digital   vs sensor[t]
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            if (a.chessnut_sensor_baseline_set &&
                prev[r][c] != sensor[r][c]) {
                deltas.push_back(Diff{r, c, prev[r][c], sensor[r][c]});
            }
            if (digital[r][c] != sensor[r][c]) {
                digital_diffs.push_back(
                    Diff{r, c, digital[r][c], sensor[r][c]});
            }
        }
    }

    std::fprintf(stderr,
        "[chessnut/sensor] frame delta=%zu digital_diff=%zu (mode=%d "
        "two_player=%d white_turn=%d settling=%d baseline_set=%d)\n",
        deltas.size(), digital_diffs.size(),
        static_cast<int>(a.mode),
        a.two_player_mode ? 1 : 0,
        a.game.white_turn ? 1 : 0,
        settling ? 1 : 0,
        a.chessnut_sensor_baseline_set ? 1 : 0);
    // Dump three grids: digital, current sensor, and 'd' marks for
    // squares disagreeing with digital + 'X' marks for squares that
    // changed since the previous sensor frame (the actual move).
    auto fmt_cell = [](char c) -> char { return (c && c != ' ') ? c : '.'; };
    std::fprintf(stderr, "      digital     sensor     diff\n");
    for (int r = 7; r >= 0; r--) {  // rank 8 → rank 1
        char dig[9] = "        ";
        char sen[9] = "        ";
        char dif[9] = "        ";
        for (int file = 0; file < 8; file++) {
            int col = 7 - file;  // file 0 = a = our col 7
            dig[file] = fmt_cell(digital[r][col]);
            sen[file] = fmt_cell(sensor[r][col]);
            char mark = '.';
            if (digital[r][col] != sensor[r][col]) mark = 'd';
            if (a.chessnut_sensor_baseline_set &&
                prev[r][col] != sensor[r][col]) mark = 'X';
            dif[file] = mark;
        }
        std::fprintf(stderr, "  %d   %s     %s     %s\n", r + 1, dig, sen, dif);
    }
    for (const auto& d : deltas) {
        std::fprintf(stderr,
            "  delta col=%d row=%d (%c%c) prev='%c' now='%c'\n",
            d.col, d.row,
            static_cast<char>('a' + (7 - d.col)),
            static_cast<char>('1' + d.row),
            d.before ? d.before : '?',
            d.after  ? d.after  : '?');
    }

    // Categorise digital_diffs into a "missing" list (digital has
    // piece, sensor empty), an "extra" list (digital empty, sensor
    // has piece), and pair them by piece type. A paired
    // missing+extra of the same type is a piece that has *moved*
    // (e.g. mid-pickup-then-drop, or a settled move whose pickup
    // and drop landed on separate sensor frames). Unpaired pieces
    // are the actual board-vs-digital desync.
    struct PairedMove { int from_idx; int to_idx; };  // indices into digital_diffs
    std::vector<int>        unpaired_missing_idx;
    std::vector<int>        unpaired_extra_idx;
    std::vector<PairedMove> paired_moves;
    {
        std::vector<int> miss_idx, extra_idx;
        for (size_t i = 0; i < digital_diffs.size(); i++) {
            const auto& d = digital_diffs[i];
            if (d.before != ' ' && d.after == ' ')
                miss_idx.push_back(static_cast<int>(i));
            else if (d.before == ' ' && d.after != ' ')
                extra_idx.push_back(static_cast<int>(i));
            // d.before != ' ' && d.after != ' ' && before != after
            // is a "wrong piece" (promotion / sensor glitch); we
            // leave it as-is and don't count it as missing/extra.
        }
        std::vector<bool> extra_used(extra_idx.size(), false);
        for (int mi : miss_idx) {
            char piece = digital_diffs[mi].before;
            int matched_extra = -1;
            for (size_t ei = 0; ei < extra_idx.size(); ei++) {
                if (extra_used[ei]) continue;
                if (digital_diffs[extra_idx[ei]].after == piece) {
                    matched_extra = static_cast<int>(ei);
                    break;
                }
            }
            if (matched_extra >= 0) {
                extra_used[matched_extra] = true;
                paired_moves.push_back({mi, extra_idx[matched_extra]});
            } else {
                unpaired_missing_idx.push_back(mi);
            }
        }
        for (size_t ei = 0; ei < extra_idx.size(); ei++) {
            if (!extra_used[ei]) unpaired_extra_idx.push_back(extra_idx[ei]);
        }
    }

    // Maintain the board-disagreement modal. `can_open` distinguishes
    // "first stable frame after game start" (allowed to open the
    // modal) from "every later stable frame mid-game" (only allowed
    // to close it — opening mid-game would flash spuriously during
    // pickup-vs-drop sensor transients).
    auto refresh_missing_modal = [&](bool can_open) {
        std::string missing_list;
        int n_missing = static_cast<int>(unpaired_missing_idx.size());
        for (int i = 0; i < n_missing && i < 8; i++) {
            const auto& d = digital_diffs[unpaired_missing_idx[i]];
            if (i > 0) missing_list += ", ";
            missing_list += static_cast<char>('a' + (7 - d.col));
            missing_list += static_cast<char>('1' + d.row);
        }
        if (n_missing > 8) missing_list += "…";

        // Decide what state the modal *should* be in.
        bool want_open = false;
        AppState::ChessnutModalType want_type = AppState::ChessnutModalType::Missing;
        std::string want_status;
        if (n_missing > 0) {
            want_open = true;
            want_type = AppState::ChessnutModalType::Missing;
            want_status = "Chessnut: pieces missing on " + missing_list +
                          " — place them or check the piece battery";
        } else if (!paired_moves.empty() || !unpaired_extra_idx.empty()) {
            // Every piece is detected somewhere on the board, but
            // the layout doesn't match the digital starting
            // position. Mid-game this gets handled by the move-
            // detection paths below (paired_moves get applied as
            // moves), so we only open the WrongLayout modal at
            // game start.
            want_open = true;
            want_type = AppState::ChessnutModalType::WrongLayout;
            want_status = "Chessnut: place pieces in the starting position";
        }

        const bool was_open = a.chessnut_missing_modal_open;
        if (want_open && (was_open || can_open)) {
            a.chessnut_missing_modal_open  = true;
            a.chessnut_missing_modal_type  = want_type;
            a.chessnut_missing_squares_msg = missing_list;
            if (!was_open) {
                set_status(a, want_status.c_str());
                audio_play(SoundEffect::Mistake);
            }
            queue_redraw(a);
        } else if (!want_open && was_open) {
            a.chessnut_missing_modal_open  = false;
            a.chessnut_missing_squares_msg.clear();
            a.chessnut_missing_exit_hover  = false;
            set_status(a, "Chessnut: all pieces detected");
            queue_redraw(a);
            // If the human is playing black, Stockfish was held
            // back so it wouldn't race the motors. Now that the
            // board is positioned, kick it off.
            if (a.chessnut_pending_ai_trigger) {
                a.chessnut_pending_ai_trigger = false;
                if (a.mode == MODE_PLAYING && !a.game.game_over &&
                    !a.two_player_mode &&
                    a.game.white_turn != a.human_plays_white) {
                    trigger_ai(a);
                }
            }
        }
    };

    // First frame after enable / connect / new game: nothing to
    // diff against. Wait for motors to finish (settling window
    // closed) before snapshotting the baseline — otherwise we'd
    // capture an in-flight transient and the missing-piece check
    // below would fire on every game start while pieces are still
    // moving.
    if (!a.chessnut_sensor_baseline_set) {
        if (settling) {
            std::fprintf(stderr,
                "[chessnut/sensor] motors settling, baseline pending\n");
            return;
        }
        a.chessnut_sensor_baseline_set = true;
        std::fprintf(stderr,
            "[chessnut/sensor] baseline established (%zu disagreements with digital)\n",
            digital_diffs.size());
        refresh_missing_modal(/*can_open=*/true);
        return;
    }

    if (deltas.empty()) {
        // Stable frame. Two things to check.
        //
        // 1) A "settled move" — pickup and drop landed in different
        //    sensor frames so the per-frame delta was 1 each time
        //    (filtered as mid-pickup). The completed move shows up
        //    here as exactly one paired missing+extra in
        //    digital_diffs. Promote that pair to a synthesised
        //    delta and fall through to the normal apply path so
        //    the move actually registers.
        //
        // 2) Modal refresh — close it if conditions resolved.
        //    can_open=false so we don't flash the modal during
        //    transient pickup states; the modal can only be opened
        //    via the establish-baseline path above.
        if (!settling && paired_moves.size() == 1 &&
            unpaired_missing_idx.empty() && unpaired_extra_idx.empty() &&
            !a.game.ai_animating) {
            const auto& md = digital_diffs[paired_moves[0].from_idx];
            const auto& ed = digital_diffs[paired_moves[0].to_idx];
            // Pre-validate the implied move's legality before
            // synthesising — if it isn't legal, this paired pair is
            // almost certainly a motor-in-transit transient (e.g.
            // a pawn glided through e3 on its way from e2 to e4).
            // Without this check the apply path would reject the
            // bogus move as "illegal" and fire the chess-violation
            // shake on every motor move that bumps a piece across
            // an intermediate square.
            bool legal = false;
            int piece_idx = a.game.grid[md.row][md.col];
            int side_to_move_white = a.game.white_turn ? 1 : 0;
            if (piece_idx >= 0 &&
                (a.game.pieces[piece_idx].is_white ? 1 : 0) == side_to_move_white) {
                auto legal_moves = generate_legal_moves(a.game, md.col, md.row);
                for (const auto& [mc, mr] : legal_moves) {
                    if (mc == ed.col && mr == ed.row) { legal = true; break; }
                }
            }
            if (legal) {
                std::fprintf(stderr,
                    "[chessnut/sensor] settled move detected via digital_diff: %c%c%c%c\n",
                    static_cast<char>('a' + (7 - md.col)),
                    static_cast<char>('1' + md.row),
                    static_cast<char>('a' + (7 - ed.col)),
                    static_cast<char>('1' + ed.row));
                deltas.push_back(Diff{md.row, md.col, md.before, ' '});
                deltas.push_back(Diff{ed.row, ed.col, ' ',       ed.after});
            } else {
                std::fprintf(stderr,
                    "[chessnut/sensor] paired pair %c%c→%c%c not legal — "
                    "treating as motor-in-transit, no action\n",
                    static_cast<char>('a' + (7 - md.col)),
                    static_cast<char>('1' + md.row),
                    static_cast<char>('a' + (7 - ed.col)),
                    static_cast<char>('1' + ed.row));
            }
        }
        if (deltas.empty()) {
            if (!settling) refresh_missing_modal(/*can_open=*/false);
            return;
        }
        // Fall through with synthesised deltas to the apply path.
    }

    if (deltas.size() == 1) {
        // Mid-pickup intermediate state (piece in the user's hand,
        // sensor square empty). Wait for the landing.
        std::fprintf(stderr,
            "[chessnut/sensor] 1-delta (mid-pickup) — waiting for landing\n");
        return;
    }

    // While motors are mid-motion the firmware feeds us in-flight
    // frames that look like 2-square deltas — but they're our own
    // animation, not user moves. Don't try to interpret.
    if (settling) {
        std::fprintf(stderr,
            "[chessnut/sensor] in settling window (%lld us since last sync) — ignoring\n",
            static_cast<long long>(now - a.chessnut_last_sync_us));
        return;
    }

    // Modal currently up — re-check whether it should still be
    // there given the latest sensor state, then ignore the delta
    // as a move attempt (game is paused while modal is up).
    if (a.chessnut_missing_modal_open) {
        std::fprintf(stderr,
            "[chessnut/sensor] missing-pieces modal open — ignoring delta\n");
        refresh_missing_modal(/*can_open=*/false);
        return;
    }

    // Reject a sensor frame: status + SFX + force-sync (motors push
    // the piece back to its digital-state square) in both single-
    // and two-player modes. The damped board shake fires only for
    // genuine chess-rule violations — moving the wrong colour or
    // making an illegal move. Sensor-noise rejections (multi-
    // piece junk, no clear from/to, off-turn timing) skip the
    // shake because the user often hasn't actually attempted any
    // move at all.
    auto reject = [&](const char* reason, bool invalid_chess_move) {
        std::fprintf(stderr,
            "[chessnut/sensor] reject: %s — delta=%zu\n",
            reason, deltas.size());
        std::string msg = std::string("Chessnut: ") + reason +
                          " — re-syncing board";
        set_status(a, msg.c_str());
        audio_play(SoundEffect::Mistake);
        if (invalid_chess_move) a.board_shake_start_us = now_us(a);
        // Force-sync drives pieces back to the digital state. The
        // next ~2 s of NOTIFY frames land in the settling window
        // above and are ignored.
        app_chessnut_sync_board(a, /*force=*/true);
    };

    // Only handle the simple-move pattern automatically: exactly
    // two squares changed — one piece left, one piece arrived —
    // and the moved piece type+colour matches between source and
    // destination. Castling (4 deltas) and en passant (3 deltas)
    // would also need handling but are rarer; deferring those.
    if (deltas.size() != 2) {
        reject("unrecognised state (multi-piece change)", /*invalid_chess_move=*/false);
        return;
    }

    // Identify which side moved. The "from" square is the one that
    // was occupied in the previous sensor frame and is empty now;
    // the "to" square is the one whose new value matches the piece
    // that disappeared from the "from" square.
    Diff* from = nullptr;
    Diff* to   = nullptr;
    for (auto& d : deltas) {
        if (d.before != ' ' && d.after == ' ') from = &d;
        else                                    to   = &d;
    }
    if (!from || !to) {
        reject("unrecognised state (no clear from/to)", /*invalid_chess_move=*/false);
        return;
    }
    if (from->before != to->after) {
        // Not a piece "moved" pattern — could be promotion or a
        // sensor glitch where two unrelated squares changed.
        reject("piece type mismatch (promotion or sensor glitch)", /*invalid_chess_move=*/false);
        return;
    }

    if (!sensor_action_allowed(a)) {
        std::fprintf(stderr,
            "[chessnut/sensor] move detected but app state blocks "
            "auto-apply (mode=%d ai_thinking=%d game_over=%d)\n",
            static_cast<int>(a.mode),
            a.game.ai_thinking ? 1 : 0,
            a.game.game_over ? 1 : 0);
        reject("not your turn / game not active", /*invalid_chess_move=*/false);
        return;
    }

    // Validate as a legal move for the side currently to move. If
    // the user moved a piece for the OTHER side, we'd refuse here
    // — that's intentional; we don't want the board's accidental
    // touch of the wrong piece to corrupt the game.
    GameState& gs = a.game;
    int side_to_move_white = gs.white_turn ? 1 : 0;
    int piece_idx = gs.grid[from->row][from->col];
    if (piece_idx < 0) {
        // No piece at the "from" square in the digital state.
        // Almost always a sensor-noise / motor-in-transit artefact
        // (e.g. a pawn glided through e3 on its way from e2 to e4
        // and the previous frame caught it mid-glide, so now we
        // see "e3 lost piece" but digitally e3 has been empty all
        // along). Force-sync the board back, but don't shake —
        // the user didn't actually attempt anything wrong.
        reject("no piece at source square", /*invalid_chess_move=*/false);
        return;
    }
    if ((gs.pieces[piece_idx].is_white ? 1 : 0) != side_to_move_white) {
        reject("wrong side moved", /*invalid_chess_move=*/true);
        return;
    }
    auto legal = generate_legal_moves(gs, from->col, from->row);
    bool ok = false;
    for (const auto& [mc, mr] : legal) {
        if (mc == to->col && mr == to->row) { ok = true; break; }
    }
    if (!ok) {
        reject("illegal move", /*invalid_chess_move=*/true);
        return;
    }

    bool capture = gs.grid[to->row][to->col] >= 0;
    std::fprintf(stderr,
        "[chessnut/sensor] applying move %c%c%c%c (capture=%d)\n",
        static_cast<char>('a' + (7 - from->col)),
        static_cast<char>('1' + from->row),
        static_cast<char>('a' + (7 - to->col)),
        static_cast<char>('1' + to->row),
        capture ? 1 : 0);

    // Both single- and two-player sensor moves go through the
    // animation pipeline so the user sees the same arrow + piece-
    // flying visual that AI moves produce. ai_anim_skip_chessnut_sync
    // tells tick_ai_animation to skip the post-animation sync_board
    // (the piece is already at its destination on the physical
    // board). ai_anim_trigger_ai_after kicks Stockfish off in
    // single-player after execute_move runs there. Both flags are
    // cleared by tick_ai_animation when consumed.
    gs.selected_col = gs.selected_row = -1;
    gs.valid_moves.clear();
    gs.ai_anim_skip_chessnut_sync = true;
    gs.ai_anim_trigger_ai_after   = !a.two_player_mode;
    start_ai_animation(a, from->col, from->row, to->col, to->row);
    (void)capture;
}

void app_chessnut_apply_status(AppState& a, const std::string& status) {
    // The picker is shown even when chessnut_enabled is false —
    // we open the picker BEFORE flipping the toggle on. Don't
    // gate DEVICE / SCAN_COMPLETE behind chessnut_enabled.
    if (status.rfind("DEVICE ", 0) == 0) {
        // Format: "DEVICE <mac> <name…>"  (name may contain spaces)
        std::string rest = status.substr(7);
        size_t sp = rest.find(' ');
        AppState::ChessnutScannedDevice d;
        if (sp == std::string::npos) {
            d.address = rest;
        } else {
            d.address = rest.substr(0, sp);
            d.name    = rest.substr(sp + 1);
        }
        if (!d.address.empty()) {
            // De-dup: if we already have a row for this MAC, keep
            // the first one (it's likely got the same name and we
            // don't want the picker rows to flicker).
            bool already = false;
            for (const auto& e : a.chessnut_devices) {
                if (e.address == d.address) { already = true; break; }
            }
            if (!already) a.chessnut_devices.push_back(std::move(d));
        }
        queue_redraw(a);
        return;
    }
    if (status == "SCAN_COMPLETE") {
        a.chessnut_picker_scanning = false;
        set_status(a, a.chessnut_devices.empty()
            ? "Chessnut Move: no devices found"
            : "Chessnut Move: pick a device below");
        queue_redraw(a);
        return;
    }
    if (status.rfind("NOTIFY ", 0) == 0) {
        // Format: "NOTIFY <uuid> <hex>". Route based on payload
        // size into the right protocol's sensor handler:
        //
        //   * Chessnut Move: 38-byte FEN-data frame = 76 hex chars
        //     (apply_sensor_frame validates the 0x01 0x24 header
        //     and drops non-board frames).
        //   * Phantom: 9-byte detected-move frame = 18 hex chars
        //     starting with the literal "M 1 " prefix.
        std::string rest = status.substr(7);
        size_t sp = rest.find(' ');
        if (sp == std::string::npos) return;
        std::string hex = rest.substr(sp + 1);
        // Verbose-log surfacing — when the user has the toggle on,
        // route the raw frame into the status bar so they can
        // capture frames without terminal access. Truncate the UUID
        // to its 8-char prefix and the hex to a screen-friendly
        // length so the line fits the status bar at typical widths.
        if (a.ble_verbose_log) {
            std::string uuid = rest.substr(0, sp);
            std::string uuid_short = uuid.substr(0, 8);
            std::string hex_show = hex.size() > 64
                ? (hex.substr(0, 60) + "…")
                : hex;
            std::string msg = "BLE " + uuid_short + " " + hex_show;
            set_status(a, msg.c_str());
            queue_redraw(a);
        }
        if (hex.size() >= chessnut::FEN_DATA_HEX_CHARS) {
            app_chessnut_apply_sensor_frame(a, hex);
            return;
        }
        // Phantom detected-move parse: 18 hex chars, ASCII payload
        // with the verified `"M 1 "` prefix.
        if (hex.size() == phantom::DETECTED_MOVE_FRAME_LEN * 2 &&
            a.chessnut_board_kind == AppState::ChessnutBoardKind::Phantom) {
            uint8_t bytes[phantom::DETECTED_MOVE_FRAME_LEN];
            for (size_t i = 0; i < phantom::DETECTED_MOVE_FRAME_LEN; ++i) {
                unsigned v = 0;
                std::sscanf(hex.substr(i * 2, 2).c_str(), "%x", &v);
                bytes[i] = static_cast<uint8_t>(v);
            }
            int sc, sr, dc, dr; bool capture;
            if (phantom::parse_detected_move(bytes,
                    phantom::DETECTED_MOVE_FRAME_LEN,
                    sc, sr, dc, dr, capture)) {
                std::fprintf(stderr,
                    "[phantom/sensor] detected move %c%c%c%c%c "
                    "(capture=%d)\n",
                    'a' + sc, '1' + sr, capture ? 'x' : '-',
                    'a' + dc, '1' + dr, capture ? 1 : 0);
                // TODO: feed (sc,sr)→(dc,dr) into the digital game
                // via execute_move() once the modal / animation
                // wiring is in place. For now we just log.
            }
        }
        return;
    }
    if (!a.chessnut_enabled && !a.chessnut_picker_open) return;
    auto board_label = [&]() {
        return a.chessnut_board_kind == AppState::ChessnutBoardKind::Phantom
            ? "Phantom" : "Chessnut Move";
    };
    if (status.rfind("CONNECTED", 0) == 0) {
        // Web build doesn't go through pick_device — the browser
        // owns its picker — so the board kind hasn't been set yet.
        // Infer it from the connected device's name (the bridge
        // emits "CONNECTED <name>"), which matches one of the two
        // protocol families' advertising-name keywords.
        if (status.size() > 10) {
            std::string device_name = status.substr(10);  // skip "CONNECTED "
            if (phantom::is_phantom_name(device_name)) {
                a.chessnut_board_kind = AppState::ChessnutBoardKind::Phantom;
            } else {
                a.chessnut_board_kind = AppState::ChessnutBoardKind::Move;
            }
        }
        a.chessnut_connected = true;
        a.chessnut_picker_open = false;  // close picker on success
        a.chessnut_picker_scanning = false;
        a.chessnut_devices.clear();
        std::string msg = std::string(board_label()) + ": " + status;
        set_status(a, msg.c_str());
        // Initial sync on connect — push current position with
        // force=1 so the firmware always replans from sensor state.
        // (For Phantom, sync_board's force path is a no-op — the
        // board has no setMoveBoard primitive — but calling it
        // keeps the timestamp bookkeeping consistent.)
        app_chessnut_sync_board(a, /*force=*/true);
    } else if (status == "DISCONNECTED") {
        a.chessnut_connected = false;
        set_status(a, (std::string(board_label()) + ": disconnected").c_str());
    } else if (status.rfind("READY", 0) == 0) {
        // Helper booted; nothing more to do here.
    } else if (status.rfind("ACK ", 0) == 0) {
        // Successful command — no UI noise needed.
    } else if (status.rfind("ERROR ", 0) == 0 ||
               status.rfind("FATAL ", 0) == 0) {
        std::string msg = std::string(board_label()) + ": " + status;
        set_status(a, msg.c_str());
    }
    queue_redraw(a);
}

#ifndef __EMSCRIPTEN__
namespace {
// Bridge instances owned here so their destructors reap the
// SimpleBLE / subprocess resources on app exit. Only one of the
// two is connected at a time — the one matching the picked
// device's protocol. The other stays null until the user picks
// a device of that type. `g_active_bridge` is the polymorphic
// pointer used for per-move dispatch; it always points at one
// of the two unique_ptrs above (whichever is currently connected)
// or nullptr while we're between connections.
std::unique_ptr<ChessnutBridge> g_chessnut_bridge;
std::unique_ptr<PhantomBridge>  g_phantom_bridge;
IBoardBridge*                   g_active_bridge = nullptr;

// Last on_status callback supplied to set_enabled, kept here so the
// picker entry points (open / pick / close) can re-use the same
// status routing without forcing every caller to thread the
// callback through. Shared between the two bridges — both emit
// the same status-line vocabulary ("READY", "DEVICE …",
// "CONNECTED …", "DISCONNECTED", "ERROR …").
std::function<void(const std::string&)> g_chessnut_status_cb;
}  // namespace

// Make sure the bridge subprocess / SimpleBLE worker is alive and
// the status callback is wired. Returns true if the bridge is
// usable after this call.
static bool ensure_chessnut_bridge(
    AppState& a,
    std::function<void(const std::string&)> on_status) {
    if (!g_chessnut_bridge) g_chessnut_bridge = std::make_unique<ChessnutBridge>();
    if (g_chessnut_bridge->running()) {
        // Already running from a previous toggle — just refresh the
        // callback so subsequent status updates land here.
        if (on_status) g_chessnut_status_cb = on_status;
        return true;
    }
    if (on_status) g_chessnut_status_cb = on_status;
    if (!g_chessnut_bridge->start(g_chessnut_status_cb)) {
        g_chessnut_bridge.reset();
        set_status(a, "Chessnut Move: bridge failed to start");
        queue_redraw(a);
        return false;
    }
    g_active_bridge = g_chessnut_bridge.get();
    a.chessnut_bridge_running = true;
    return true;
}

// Mirror of ensure_chessnut_bridge for the Phantom driver. Only one
// of the two bridges is alive at a time — the picker chooses which
// based on the selected device's advertised name.
static bool ensure_phantom_bridge(
    AppState& a,
    std::function<void(const std::string&)> on_status) {
    if (!g_phantom_bridge) g_phantom_bridge = std::make_unique<PhantomBridge>();
    if (g_phantom_bridge->running()) {
        if (on_status) g_chessnut_status_cb = on_status;
        return true;
    }
    if (on_status) g_chessnut_status_cb = on_status;
    if (!g_phantom_bridge->start(g_chessnut_status_cb)) {
        g_phantom_bridge.reset();
        set_status(a, "Phantom: bridge failed to start");
        queue_redraw(a);
        return false;
    }
    g_active_bridge = g_phantom_bridge.get();
    a.chessnut_bridge_running = true;
    return true;
}

void app_chessnut_set_enabled(
    AppState& a, bool on,
    std::function<void(const std::string& status)> on_status) {
    if (on == a.chessnut_enabled) return;

    if (!on) {
        g_active_bridge = nullptr;
        if (g_chessnut_bridge) g_chessnut_bridge->stop();
        g_chessnut_bridge.reset();
        if (g_phantom_bridge)  g_phantom_bridge->stop();
        g_phantom_bridge.reset();
        g_chessnut_status_cb = nullptr;
        a.chessnut_enabled        = false;
        a.chessnut_bridge_running = false;
        a.chessnut_connected      = false;
        a.chessnut_picker_open    = false;
        a.chessnut_picker_scanning = false;
        a.chessnut_devices.clear();
        a.chessnut_board_kind = AppState::ChessnutBoardKind::Move;
        set_status(a, "Chessnut Move: off");
        app_settings_save(a);
        queue_redraw(a);
        return;
    }

    if (!ensure_chessnut_bridge(a, std::move(on_status))) return;
    a.chessnut_enabled   = true;
    a.chessnut_connected = false;
    app_settings_save(a);

    // If we already have a cached MAC (from a previous successful
    // session or CHESS_CHESSNUT_ADDRESS env var), skip the picker
    // and connect directly. Otherwise open the picker so the user
    // can choose which board to bind to.
    set_status(a, "Chessnut Move: scanning…");
    queue_redraw(a);
    a.chessnut_devices.clear();
    a.chessnut_picker_open    = true;
    a.chessnut_picker_scanning = true;
    a.chessnut_picker_hover   = -1;
    g_chessnut_bridge->start_scan();
}

void app_chessnut_open_picker(AppState& a) {
    if (!ensure_chessnut_bridge(a, g_chessnut_status_cb)) return;
    a.chessnut_devices.clear();
    a.chessnut_picker_open     = true;
    a.chessnut_picker_scanning = true;
    a.chessnut_picker_hover    = -1;
    set_status(a, "Chessnut Move: scanning…");
    queue_redraw(a);
    g_chessnut_bridge->start_scan();
}

void app_chessnut_pick_device(AppState& a, const std::string& address) {
    // Look up the picked device's advertised name — the keyword
    // determines which protocol we route writes through. Both
    // Chessnut Move and Phantom Chessboard advertisements get
    // surfaced by the (chessnut-side) picker scan because the scan
    // filter matches both name families; we only learn the kind
    // here, at pick time.
    std::string name;
    for (const auto& d : a.chessnut_devices) {
        if (d.address == address) { name = d.name; break; }
    }
    bool is_phantom = phantom::is_phantom_name(name);

    a.chessnut_picker_open     = false;
    a.chessnut_picker_scanning = false;
    a.chessnut_picker_hover    = -1;
    a.chessnut_enabled         = true;
    a.chessnut_connected       = false;

    if (is_phantom) {
        // Stop the chessnut bridge — it was used for the scan; we
        // don't need it for ongoing chat with a Phantom. Then start
        // the Phantom bridge and connect.
        if (g_chessnut_bridge) {
            g_chessnut_bridge->stop();
            g_chessnut_bridge.reset();
        }
        a.chessnut_board_kind = AppState::ChessnutBoardKind::Phantom;
        if (!ensure_phantom_bridge(a, g_chessnut_status_cb)) return;
        g_active_bridge = g_phantom_bridge.get();
        set_status(a, ("Phantom: connecting to " + address + "…").c_str());
        queue_redraw(a);
        g_phantom_bridge->connect_to_address(address);
        return;
    }

    a.chessnut_board_kind = AppState::ChessnutBoardKind::Move;
    if (!ensure_chessnut_bridge(a, g_chessnut_status_cb)) return;
    g_active_bridge = g_chessnut_bridge.get();
    set_status(a, ("Chessnut Move: connecting to " + address + "…").c_str());
    queue_redraw(a);
    g_chessnut_bridge->connect_to_address(address);
}

void app_chessnut_close_picker(AppState& a) {
    a.chessnut_picker_open     = false;
    a.chessnut_picker_scanning = false;
    a.chessnut_picker_hover    = -1;
    a.chessnut_devices.clear();
    if (!a.chessnut_connected) {
        a.chessnut_enabled = false;
        g_active_bridge = nullptr;
        if (g_chessnut_bridge) g_chessnut_bridge->stop();
        g_chessnut_bridge.reset();
        if (g_phantom_bridge)  g_phantom_bridge->stop();
        g_phantom_bridge.reset();
        g_chessnut_status_cb = nullptr;
        a.chessnut_bridge_running = false;
        set_status(a, "Chessnut Move: off");
    }
    queue_redraw(a);
}

// Polymorphic per-move dispatcher. The shared layer doesn't care
// which protocol the connected board speaks; each concrete
// IBoardBridge implementation handles the wire-format encoding
// internally. Capture detection runs once here and the result is
// passed to whichever protocol cares (Phantom uses it to lift the
// captured piece via comerVersion3 before driving the mover).
void app_chessnut_sync_board(AppState& a, bool force) {
    if (!g_active_bridge || !a.chessnut_connected) {
        std::fprintf(stderr,
            "[bridge/sync] skip: bridge=%p connected=%d\n",
            static_cast<void*>(g_active_bridge),
            a.chessnut_connected ? 1 : 0);
        return;
    }
    std::string fen = app_current_fen(a);

    if (force) {
        std::fprintf(stderr, "[bridge/sync] %s on_full_position_set fen=%s\n",
                     g_active_bridge->label(), fen.c_str());
        g_active_bridge->on_full_position_set(fen);
        a.chessnut_last_sync_us = now_us(a);
        app_chessnut_highlight_last_move(a);
        return;
    }
    if (a.game.move_history.empty()) return;
    const std::string& uci = a.game.move_history.back();
    int fc = -1, fr = -1, tc = -1, tr = -1;
    if (!parse_uci_move(uci, fc, fr, tc, tr)) return;
    // Capture detection by alive-piece delta between the last two
    // snapshots — handles regular captures and en passant uniformly.
    bool capture = false;
    const auto& snaps = a.game.snapshots;
    if (snaps.size() >= 2) {
        int n_before = 0, n_after = 0;
        for (const auto& p : snaps[snaps.size() - 2].pieces)
            if (p.alive) n_before++;
        for (const auto& p : snaps.back().pieces)
            if (p.alive) n_after++;
        capture = (n_after < n_before);
    }
    std::fprintf(stderr, "[bridge/sync] %s on_move_played %s capture=%d\n",
                 g_active_bridge->label(), uci.c_str(), capture ? 1 : 0);
    g_active_bridge->on_move_played(fen, fc, fr, tc, tr, capture);
    a.chessnut_last_sync_us = now_us(a);
    app_chessnut_highlight_last_move(a);
}

// Light up the from + to squares of the last move via whatever
// protocol the active bridge supports. Chessnut Move drives an RGB
// LED grid; Phantom no-ops (no verified per-move LED API). An empty
// move history clears the highlight.
void app_chessnut_highlight_last_move(AppState& a) {
    if (!g_active_bridge || !a.chessnut_connected) return;
    int fc = -1, fr = -1, tc = -1, tr = -1;
    if (!a.game.move_history.empty()) {
        parse_uci_move(a.game.move_history.back(), fc, fr, tc, tr);
    }
    g_active_bridge->on_highlight_move(fc, fr, tc, tr);
}

void app_chessnut_shutdown(AppState& a) {
    a.chessnut_enabled        = false;
    a.chessnut_bridge_running = false;
    a.chessnut_connected      = false;
    g_active_bridge = nullptr;
    if (g_chessnut_bridge) g_chessnut_bridge->stop();
    g_chessnut_bridge.reset();
    if (g_phantom_bridge)  g_phantom_bridge->stop();
    g_phantom_bridge.reset();
}

void app_chessnut_forget_cached_device(AppState& a) {
    // Match the cache path used by chessnut_bridge.cpp's
    // load_cached_address (and the Python helper's identical
    // ADDRESS_CACHE in tools/chessnut_bridge.py).
    const char* home = std::getenv("HOME");
    if (home && *home) {
        std::string path =
            std::string(home) + "/.cache/chessnut_bridge_address";
        std::remove(path.c_str());
    }
    set_status(a, "Chessnut Move: forgot cached device — rescanning…");
    queue_redraw(a);
    // If the picker is open, refresh the device list with a new
    // scan. If it isn't, that's fine — the next toggle-on will
    // pick up the missing cache and pop the picker as usual.
    if (a.chessnut_picker_open && g_chessnut_bridge &&
        g_chessnut_bridge->running()) {
        a.chessnut_devices.clear();
        a.chessnut_picker_scanning = true;
        a.chessnut_picker_hover    = -1;
        g_chessnut_bridge->start_scan();
    }
}

static void chessnut_tick_reconnect(AppState& a, int64_t now) {
    // Auto-reconnect to the Chessnut Move while the toggle is on.
    // The board may sleep / drop the BLE link while the user is
    // away from the desk; without this the toggle stays "on" but
    // every write silently fails. Retry every kChessnutReconnectUs
    // with the cached MAC (the bridge falls back to it first,
    // so it's a fast 2.5 s scan rather than the 8 s name scan).
    constexpr int64_t kChessnutReconnectUs = 30LL * 1000LL * 1000LL;  // 30 s
    // Auto-reconnect is Move-only for now. The Phantom bridge has
    // no name-prefix scan path (it only connects by explicit
    // address) so we can't reissue a "connect to whichever
    // Phantom is around" without remembering the address — left
    // for a follow-up. Phantom users see a plain "DISCONNECTED"
    // status and need to re-pair via the picker.
    if (a.chessnut_board_kind == AppState::ChessnutBoardKind::Phantom) return;
    if (a.chessnut_enabled && !a.chessnut_connected &&
        !a.chessnut_picker_open &&
        g_chessnut_bridge && g_chessnut_bridge->running()) {
        if (a.chessnut_last_reconnect_us == 0 ||
            now - a.chessnut_last_reconnect_us > kChessnutReconnectUs) {
            std::fprintf(stderr,
                "[chessnut/reconnect] retrying after disconnect\n");
            a.chessnut_last_reconnect_us = now;
            g_chessnut_bridge->request_connect();
        }
    } else if (a.chessnut_connected && a.chessnut_last_reconnect_us != 0) {
        // Reset the throttle once a reconnect has succeeded so the
        // next disconnect doesn't have to wait the full window.
        a.chessnut_last_reconnect_us = 0;
    }
}
#endif  // !__EMSCRIPTEN__

// ===========================================================================
// Lifecycle
// ===========================================================================
void app_init(AppState& a, const AppPlatform* platform) {
    a.platform = platform;
    game_reset(a.game);
    app_settings_load(a);
}

// ===========================================================================
// Settings persistence — minimal key=value INI at
// $XDG_CONFIG_HOME/3d_chess/settings.ini (or ~/.config/3d_chess/...).
// Web stubs both calls in chessnut_web.cpp / voice_web.cpp area.
// ===========================================================================
#ifndef __EMSCRIPTEN__
namespace {
std::string settings_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg && *xdg) {
        base = xdg;
    } else if (const char* home = std::getenv("HOME")) {
        base = std::string(home) + "/.config";
    } else {
        return std::string();
    }
    return base + "/3d_chess/settings.ini";
}

bool parse_bool(const std::string& v) {
    return v == "1" || v == "true" || v == "TRUE" ||
           v == "yes" || v == "on";
}
}  // namespace

void app_settings_load(AppState& a) {
    std::string path = settings_path();
    if (path.empty()) return;
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
            s.pop_back();
        if (s.empty() || s[0] == '#') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = s.substr(0, eq);
        std::string val = s.substr(eq + 1);
        if (key == "cartoon_outline") {
            a.cartoon_outline = parse_bool(val);
        } else if (key == "chessnut_enabled") {
            // Don't immediately turn the bridge on here — app_init
            // runs before the platform's IO is fully primed. Instead
            // record the desire; the UI driver flips the toggle
            // through app_chessnut_toggle_request once we're up.
            a.chessnut_enabled = parse_bool(val);
        }
    }
    std::fclose(f);
    // chessnut_enabled persisted as true means "the user wants the
    // toggle on at startup". The bridge subprocess hasn't started
    // yet (no platform callback wired), so reset the flag to false
    // for now — main.cpp will call app_chessnut_toggle_request once
    // the GTK loop is alive if it sees that the prior session had
    // it enabled. To pass that signal forward, stash it in a
    // separate field that app_init doesn't clear.
    // (Simpler approach implemented here: we leave a.chessnut_enabled
    //  pre-set to true, and main.cpp reads it on startup to decide
    //  whether to fire the toggle. This avoids a "ghost" state where
    //  the flag is on but the bridge isn't.)
}

void app_settings_save(const AppState& a) {
    std::string path = settings_path();
    if (path.empty()) return;
    // mkdir -p on the parent dir.
    std::string dir = path.substr(0, path.find_last_of('/'));
    if (!dir.empty()) {
        std::string cmd = "mkdir -p " + dir + " 2>/dev/null";
        int unused = std::system(cmd.c_str()); (void)unused;
    }
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "# 3d_chess user settings — auto-generated\n");
    std::fprintf(f, "cartoon_outline=%d\n", a.cartoon_outline ? 1 : 0);
    std::fprintf(f, "chessnut_enabled=%d\n", a.chessnut_enabled ? 1 : 0);
    std::fclose(f);
}
#else
void app_settings_load(AppState& /*a*/) {}
void app_settings_save(const AppState& /*a*/) {}
#endif
