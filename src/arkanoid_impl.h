#pragma once

#include "arkanoid.h"
#include <vector>
#include <string>
#include <imgui.h>

#define USE_ARKANOID_IMPL

class ArkanoidImpl : public Arkanoid
{
public:
    // Public API (overrides)
    void reset(const ArkanoidSettings& settings) override;
    void update(ImGuiIO& io, ArkanoidDebugData& debug_data, float elapsed) override;
    void draw(ImGuiIO& io, ImDrawList& draw_list) override;

private:
    // Game states
    enum class GameState { Playing, Win, Lose };

    // Brick representation (supports multi-hit bricks up to 3 HP)
    struct Brick {
        Rect rect_world;   // in world coordinates
        bool alive = true;
        int  score = 10;   // base score when destroyed
        bool bonus = false;// flagged to spawn a bonus when destroyed
        ImU32 color = IM_COL32(180, 200, 230, 255);

        // durability and base color
        int hit_points = 1;    // 1..3 hits
        ImU32 base_color = IM_COL32(180, 200, 230, 255);
    };

    // Types of bonuses / powerups
    enum class BonusType { SpeedUp, EnlargePaddle, ExtraLife, Pierce, SlowMo, Points, Magnet, ScoreMult, NukeRow };

    // Falling bonus item
    struct Bonus {
        Rect rect_world;
        BonusType type;
        Vect vel;
        bool alive = true;
        ImU32 color = IM_COL32(255, 220, 120, 255);
        int points = 0;
        float glow = 0.0f; // visual pulsation
    };

    // Small particle for destruction visual
    struct Particle {
        Vect pos;
        Vect vel;
        float life; // seconds remaining
        float size;
        ImU32 color;
    };

    // Internal helpers (logic)
    void build_level(const ArkanoidSettings& s);
    void clamp_carriage();
    void launch_ball_if_needed();
    void integrate_ball(float dt);
    void handle_collisions(ArkanoidDebugData& debug_data);
    bool collide_ball_with_rect(const Rect& r, Vect& out_normal, Vect& out_hit_pos, float& out_t);
    void reflect_ball(const Vect& normal);
    void add_debug_hit(ArkanoidDebugData& debug_data, const Vect& world_pos, const Vect& normal);

    // Bounce logic from paddle based on hit location
    void bounce_from_carriage(const Rect& r, const Vect& hit_pos_world);

    // Cheats and controls handler
    void handle_cheats_and_controls(ImGuiIO& io, float dt);

    // Rendering helpers
    void draw_world(ImDrawList& dl);
    void draw_ui(ImGuiIO& io, ImDrawList& dl);
    void draw_cheats_panel(ImGuiIO& io);        // separate cheat/shop popup (right side)
    void draw_main_debug_menu(ImGuiIO& io);     // single combined Arkanoid (Debug) menu (center top, dropdown)
    void draw_centered_modal(ImGuiIO& io, ImDrawList& dl, const char* title, const char* msg, ImU32 color);

    // Bonus lifecycle
    void spawn_bonus_at(const Vect& world_pos, BonusType t);
    void integrate_bonuses(float dt);
    void draw_bonuses(ImDrawList& dl);
    void apply_bonus(Bonus& b);

    // Particles
    void spawn_particles(const Vect& world_pos, ImU32 color, int count = 14);
    void integrate_particles(float dt);
    void draw_particles(ImDrawList& dl);

    // Shop / money helpers
    bool try_purchase(int cost);             // attempt to buy from balance
    void grant_money_from_score();           // convert score -> money periodically
    void add_money(int amount);              // add immediate money to balance/total

    // Coordinate conversion helpers
    inline Vect world_to_screen_scale(ImGuiIO& io) const {
        return Vect(io.DisplaySize.x / world_size.x, io.DisplaySize.y / world_size.y);
    }
    inline ImVec2 to_screen(ImGuiIO& io, const Vect& w) const {
        Vect s = Vect(w.x * screen_scale.x, w.y * screen_scale.y);
        return ImVec2(s);
    }
    inline float to_screen_x(float wx) const { return wx * screen_scale.x; }
    inline float to_screen_y(float wy) const { return wy * screen_scale.y; }

private:
    // Settings & computed parameters
    ArkanoidSettings settings{};
    Vect world_size = Vect(800.0f, 600.0f);
    Vect screen_scale = Vect(1.0f, 1.0f);

    // Bricks
    std::vector<Brick> bricks;
    int bricks_cols = 0;
    int bricks_rows = 0;
    Vect brick_size = Vect(0.0f, 0.0f);
    Vect bricks_origin = Vect(0.0f, 0.0f);

    // destroyed bricks counter -> used for speedup mechanic
    int destroyed_bricks_count = 0;

    // Bonuses
    std::vector<Bonus> bonuses;

    // Particles
    std::vector<Particle> particles;

    // Paddle
    Rect carriage_world = Rect(Vect(0, 0), Vect(100, 20));
    float carriage_height = 18.0f; // paddle height (constant)
    float carriage_speed = 500.0f; // units/sec

    // Ball
    Vect ball_pos = Vect(0.0f);
    Vect ball_vel = Vect(0.0f);
    float ball_radius = 10.0f;
    float ball_speed_target = 150.0f; // target speed from settings or UI
    float ball_speed_cur = 150.0f;
    float ball_min_speed = 60.0f;     // safety floor
    float ball_max_speed = 5000.0f;   // absolute cap

    // Core game logic
    GameState state = GameState::Playing;
    int score = 0;            // current session score
    int lives = 3;
    float combo_timer = 0.0f;
    float combo_window = 1.2f; // combo window seconds
    int combo_mult = 1;

    // Effects / flags
    bool pierce_mode = false;
    float pierce_timer = 0.0f;
    float pierce_duration = 2.0f;

    bool slowmo_mode = false;
    float slowmo_timer = 0.0f;
    float slowmo_duration = 3.0f;
    float slowmo_factor = 0.45f;

    bool trail_mode = false;
    std::vector<Vect> ball_trail;

    // Cheats (UI-driven toggles)
    bool cheat_enlarge_paddle = false;
    bool cheat_extra_life = false;
    bool cheat_speed_lock = false; // kept but not exposed in cheat GUI

    // Magnet & score-multiplier powerups
    bool magnet_active = false;
    float magnet_timer = 0.0f;
    float magnet_duration = 6.0f;
    float magnet_strength = 600.0f;

    bool score_mult_active = false;
    float score_mult_timer = 0.0f;
    float score_mult_duration = 8.0f;
    int score_mult_value = 1; // 1=none, 2=x2, 3=x3

    // Cheats toggles available from shop
    bool cheat_invincible = false;
    bool cheat_freeze_ball = false;

    // Helper: ball launched flag (could be used to 'stick' the ball)
    bool ball_launched = true;

    // Pause for testing
    bool paused = false;

    // Speedup policy: every N destroyed bricks multiply speed by factor
    int bricks_to_speedup = 10;
    float speedup_factor = 1.10f; // +10%

    // Economy: currency and conversion
    int balance = 0;       // current spendable money ($)
    int total_money = 0;   // cumulative earned money
    int money_from_score_accumulator = 0; // tracks score->money conversion progress (score points)
    int score_per_dollar = 100; // 100 score -> $1

    // Shop UI state and notification
    std::string shop_message;   // last purchase message shown to player
    float shop_message_timer = 0.0f;
    float shop_message_duration = 2.5f; // seconds to display a message

};
