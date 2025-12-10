#include "arkanoid_impl.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <random>
#include <imgui.h>
#include <cmath>
#include <string>
#include <sstream>

#ifdef USE_ARKANOID_IMPL
// Factory function to create an Arkanoid instance
Arkanoid* create_arkanoid() { return new ArkanoidImpl(); }
#endif

// ----------------- Utility helpers -----------------

// Clamp a float between min (a) and max (b)
static inline float clampf(float v, float a, float b) { return std::max(a, std::min(b, v)); }

// Sign function
static inline float sgn(float v) { return (v < 0.f) ? -1.f : 1.f; }

// Compute center of rectangle
static inline Vect rect_center(const Rect& r) {
    return Vect(r.pos.x + r.size.x * 0.5f, r.pos.y + r.size.y * 0.5f);
}

// Create rectangle from x, y, width, height
static inline Rect make_rect_xywh(float x, float y, float w, float h) {
    Rect r;
    r.pos = Vect(x, y);
    r.size = Vect(w, h);
    return r;
}



// ----------------- Money & Shop -----------------

// Attempt to purchase an item, returns true if successful
bool ArkanoidImpl::try_purchase(int cost) {
    if (cost <= 0) return false;
    if (balance >= cost) {
        balance -= cost;
        shop_message = "Purchased for $" + std::to_string(cost) + "!";
        shop_message_timer = shop_message_duration;
        return true;
    }
    else {
        shop_message = "Not enough $";
        shop_message_timer = shop_message_duration;
        return false;
    }
}

// Add money to balance and total_money counters
void ArkanoidImpl::add_money(int amount) {
    if (amount <= 0) return;
    balance += amount;
    total_money += amount;
    shop_message = "Gained $" + std::to_string(amount);
    shop_message_timer = shop_message_duration;
}

// Convert score points to money incrementally
void ArkanoidImpl::grant_money_from_score() {
    if (money_from_score_accumulator < 0) money_from_score_accumulator = 0;
    if (score <= money_from_score_accumulator) return;

    int delta_points = score - money_from_score_accumulator;
    int dollars = delta_points / score_per_dollar;
    if (dollars > 0) {
        add_money(dollars);
        money_from_score_accumulator += dollars * score_per_dollar;
    }
}



// ----------------- Reset / Build Level -----------------

// Reset game state and prepare new level
void ArkanoidImpl::reset(const ArkanoidSettings& s) {
    settings = s;
    world_size = Vect(s.world_size.x, s.world_size.y);

    // Setup ball
    ball_radius = s.ball_radius;
    ball_speed_target = clampf(s.ball_speed, ArkanoidSettings::ball_speed_min, ArkanoidSettings::ball_speed_max);
    ball_speed_cur = ball_speed_target;

    // Setup paddle
    float cw = clampf(s.carriage_width, ArkanoidSettings::carriage_width_min, world_size.x * 0.95f);
    float cy = world_size.y - 40.0f;
    float cx = (world_size.x - cw) * 0.5f;
    carriage_world = make_rect_xywh(cx, cy, cw, carriage_height);

    // Ball starts above paddle
    Vect carr_center = rect_center(carriage_world);
    ball_pos = Vect(carr_center.x, carriage_world.pos.y - ball_radius - 1.0f);
    ball_vel = Vect(0.7071067f, -0.7071067f) * ball_speed_cur;
    ball_launched = true;

    // Reset game variables
    state = GameState::Playing;
    money_from_score_accumulator = 0;
    score = 0;
    balance = 0;
    lives = 3;
    combo_timer = 0.0f;
    combo_mult = 1;
    pierce_mode = false;
    pierce_timer = 0.0f;
    slowmo_mode = false;
    slowmo_timer = 0.0f;
    trail_mode = false;
    ball_trail.clear();
    bonuses.clear();
    particles.clear();

    // Reset cheats
    cheat_enlarge_paddle = false;
    cheat_extra_life = false;
    cheat_speed_lock = false;
    cheat_invincible = false;
    cheat_freeze_ball = false;

    // Reset temporary bonuses
    magnet_active = false;
    magnet_timer = 0.0f;
    score_mult_active = false;
    score_mult_timer = 0.0f;
    score_mult_value = 1;

    destroyed_bricks_count = 0;
    paused = false;

    build_level(s);
}

// Build bricks layout according to settings
void ArkanoidImpl::build_level(const ArkanoidSettings& s) {
    bricks.clear();

    bricks_cols = (int)clampf((float)s.bricks_columns_count, ArkanoidSettings::bricks_columns_min, ArkanoidSettings::bricks_columns_max);
    bricks_rows = (int)clampf((float)s.bricks_rows_count, ArkanoidSettings::bricks_rows_min, ArkanoidSettings::bricks_rows_max);

    float pad_x = clampf(s.bricks_columns_padding, ArkanoidSettings::bricks_columns_padding_min, ArkanoidSettings::bricks_columns_padding_max);
    float pad_y = clampf(s.bricks_rows_padding, ArkanoidSettings::bricks_rows_padding_min, ArkanoidSettings::bricks_rows_padding_max);

    float top_margin = 40.0f;
    float side_margin = 20.0f;
    float area_w = world_size.x - side_margin * 2.0f;
    float area_h = world_size.y * 0.45f - top_margin;

    float total_pad_x = pad_x * (bricks_cols - 1);
    float total_pad_y = pad_y * (bricks_rows - 1);
    float bw = std::max(5.0f, (area_w - total_pad_x) / bricks_cols);
    float bh = std::max(8.0f, (area_h - total_pad_y) / bricks_rows);

    brick_size = Vect(bw, bh);
    bricks_origin = Vect(side_margin, top_margin);

    // Random generators
    std::mt19937 rng(1337);
    std::uniform_real_distribution<float> unif(0.0f, 1.0f);
    std::uniform_int_distribution<int> hpDist(0, 99);

    // Populate bricks
    bricks.reserve((size_t)(bricks_cols * bricks_rows));
    for (int r = 0; r < bricks_rows; ++r) {
        for (int c = 0; c < bricks_cols; ++c) {
            float x = bricks_origin.x + c * (bw + pad_x);
            float y = bricks_origin.y + r * (bh + pad_y);
            Brick b;
            b.rect_world = make_rect_xywh(x, y, bw, bh);
            b.alive = true;
            b.score = 10 + (int)(bricks_rows - 1 - r) * 2;

            float p = unif(rng);
            b.bonus = (p < 0.15f);
            b.base_color = b.bonus ? IM_COL32(255, 200, 80, 255) : IM_COL32(140 + (int)(90.0f * r / std::max(1, bricks_rows - 1)), 180, 230, 255);

            int rnd = hpDist(rng);
            if (rnd < 5) b.hit_points = 3;
            else if (rnd < 25) b.hit_points = 2;
            else b.hit_points = 1;

            b.color = b.base_color;
            bricks.push_back(b);
        }
    }
}



// ----------------- Update / Integration -----------------

// Update game state each frame
void ArkanoidImpl::update(ImGuiIO& io, ArkanoidDebugData& debug_data, float elapsed) {
    // Compute scaling for screen rendering
    screen_scale = world_to_screen_scale(io);
    debug_data.hits.clear();

    float dt = elapsed;
    // Apply slow-motion factor if active
    if (slowmo_mode) {
        dt *= slowmo_factor;
        slowmo_timer -= elapsed;
        if (slowmo_timer <= 0.0f) {
            slowmo_timer = 0.0f;
            slowmo_mode = false;
        }
    }

    // If game is not playing, only allow restart
    if (state != GameState::Playing) {
        if (io.KeysDown[GLFW_KEY_R]) reset(settings);
        return;
    }

    grant_money_from_score();
    handle_cheats_and_controls(io, dt);

    if (paused) return;

    launch_ball_if_needed();
    integrate_ball(dt);
    integrate_bonuses(dt);
    integrate_particles(dt);
    handle_collisions(debug_data);

    // Win condition check
    bool any_alive = false;
    for (const auto& b : bricks) if (b.alive) { any_alive = true; break; }
    if (!any_alive) state = GameState::Win;
}

// Draw the full frame
void ArkanoidImpl::draw(ImGuiIO& io, ImDrawList& draw_list) {
    draw_world(draw_list);
    draw_ui(io, draw_list);

    // Show end-game modal if needed
    if (state == GameState::Win)
        draw_centered_modal(io, draw_list, "YOU WIN", "Congratulations!\nPress R to restart", IM_COL32(120, 220, 140, 255));
    else if (state == GameState::Lose)
        draw_centered_modal(io, draw_list, "YOU LOSE", "Try again!\nPress R to restart", IM_COL32(240, 120, 120, 255));

    draw_main_debug_menu(io);
}




// ----------------- Controls / Cheats -----------------
void ArkanoidImpl::handle_cheats_and_controls(ImGuiIO& io, float dt) {
    // Paddle movement
    float dx = (io.KeysDown[GLFW_KEY_D] ? 1.0f : 0.0f) - (io.KeysDown[GLFW_KEY_A] ? 1.0f : 0.0f);
    carriage_world.pos.x += dx * carriage_speed * dt;

    if (cheat_enlarge_paddle) {
        carriage_world.size.x = clampf(settings.carriage_width * 1.6f, ArkanoidSettings::carriage_width_min, world_size.x * 0.95f);
    }
    else {
        carriage_world.size.x = std::max(20.0f, carriage_world.size.x);
    }
    carriage_world.size.y = carriage_height;
    clamp_carriage();

    // Hotkeys for speed / pierce / shop purchases
    if (io.KeysDown[GLFW_KEY_1]) ball_speed_target = std::max(0.5f * ball_speed_target, ball_min_speed);
    if (io.KeysDown[GLFW_KEY_2]) ball_speed_target = settings.ball_speed;
    if (io.KeysDown[GLFW_KEY_3]) ball_speed_target = std::min(1.5f * ball_speed_target, ball_max_speed);
    if (io.KeysDown[GLFW_KEY_C]) { pierce_mode = true; pierce_timer = pierce_duration; }

    if (io.KeysDown[GLFW_KEY_X] && try_purchase(10)) { magnet_active = true; magnet_timer = magnet_duration; }
    if (io.KeysDown[GLFW_KEY_T] && try_purchase(15)) { score_mult_active = true; score_mult_timer = score_mult_duration; score_mult_value = 3; }
    if (io.KeysDown[GLFW_KEY_Q] && try_purchase(10)) cheat_freeze_ball = true;
    if (io.KeysDown[GLFW_KEY_Y] && try_purchase(60)) { cheat_invincible = true; shop_message_timer = shop_message_duration; }
    if (io.KeysDown[GLFW_KEY_E] && try_purchase(20)) lives++;

    // Nuke row cheat
    if (io.KeysDown[GLFW_KEY_N]) {
        for (auto& b : bricks) {
            if (!b.alive) continue;
            if (ball_pos.y >= b.rect_world.pos.y && ball_pos.y <= b.rect_world.pos.y + b.rect_world.size.y) {
                b.alive = false;
                score += b.score * score_mult_value;
                destroyed_bricks_count++;
                if (destroyed_bricks_count % bricks_to_speedup == 0)
                    ball_speed_target = clampf(ball_speed_target * speedup_factor, ball_min_speed, ball_max_speed);
            }
        }
    }

    // One-shot cheats
    if (cheat_extra_life) { lives++; cheat_extra_life = false; }
    if (cheat_speed_lock) ball_speed_target = ball_speed_cur;

    // Timers
    if (magnet_active) { magnet_timer -= dt; if (magnet_timer <= 0) { magnet_active = false; magnet_timer = 0; } }
    if (score_mult_active) { score_mult_timer -= dt; if (score_mult_timer <= 0) { score_mult_active = false; score_mult_value = 1; score_mult_timer = 0; } }

    // Smooth speed interpolation
    float accel = 800.0f;
    if (ball_speed_cur < ball_speed_target) ball_speed_cur = std::min(ball_speed_target, ball_speed_cur + accel * dt);
    else if (ball_speed_cur > ball_speed_target) ball_speed_cur = std::max(ball_speed_target, ball_speed_cur - accel * dt);

    // Pierce
    if (pierce_mode) { pierce_timer -= dt; if (pierce_timer <= 0) { pierce_mode = false; pierce_timer = 0; } }

    // Combo
    if (combo_timer > 0) { combo_timer -= dt; if (combo_timer <= 0) { combo_mult = 1; combo_timer = 0; } }

    // Ball trail
    if (trail_mode) { ball_trail.push_back(ball_pos); if (ball_trail.size() > 16) ball_trail.erase(ball_trail.begin()); }
    else if (!ball_trail.empty()) ball_trail.clear();

    // Shop message timer
    if (!shop_message.empty()) { shop_message_timer -= dt; if (shop_message_timer <= 0) shop_message.clear(); }
}




// ----------------- Collisions -----------------

void ArkanoidImpl::clamp_carriage() {
    carriage_world.pos.x = clampf(carriage_world.pos.x, 0.0f, world_size.x - carriage_world.size.x);
}

void ArkanoidImpl::launch_ball_if_needed() { /* Placeholder for sticky launch */ }

void ArkanoidImpl::integrate_ball(float dt) {
    static float freeze_timer = 0.0f;

    if (cheat_freeze_ball) { if (freeze_timer <= 0.0f) freeze_timer = 5.0f; cheat_freeze_ball = false; }
    if (freeze_timer > 0.0f) { freeze_timer -= dt; ball_speed_cur = std::max(ball_min_speed, ball_speed_target * 0.2f); }
    else ball_speed_cur = ball_speed_target;

    // Adjust velocity magnitude
    float cur_len = ball_vel.Length();
    if (cur_len > 1e-6f) ball_vel *= (ball_speed_cur / cur_len);

    ball_pos += ball_vel * dt;

    // Wall collisions
    if (ball_pos.x < ball_radius) { float overshoot = ball_radius - ball_pos.x; ball_pos.x += overshoot * 2.0f; reflect_ball(Vect(1, 0)); }
    else if (ball_pos.x > world_size.x - ball_radius) { float overshoot = ball_pos.x - (world_size.x - ball_radius); ball_pos.x -= overshoot * 2.0f; reflect_ball(Vect(-1, 0)); }

    if (ball_pos.y < ball_radius) { float overshoot = ball_radius - ball_pos.y; ball_pos.y += overshoot * 2.0f; reflect_ball(Vect(0, 1)); }

    // Bottom: lose life unless invincible
    if (ball_pos.y > world_size.y + ball_radius) {
        if (!cheat_invincible) lives--;
        combo_mult = 1; combo_timer = 0; pierce_mode = false; pierce_timer = 0;

        if (lives <= 0) state = GameState::Lose;
        else {
            Vect carr_center = rect_center(carriage_world);
            ball_pos = Vect(carr_center.x, carriage_world.pos.y - ball_radius - 1.0f);
            ball_vel = Vect(0.7071067f, -0.7071067f) * ball_speed_target;
        }
    }
}

// Check collision between the ball (circle) and a rectangle.
bool ArkanoidImpl::collide_ball_with_rect(
    const Rect& r,       // Rectangle to test against
    Vect& out_normal,    // Output: normal of collision surface
    Vect& out_hit_pos,   // Output: closest point on rectangle to ball center
    float& out_t         // Output: time of impact (not used here, set to 0)
) {
    // Find closest point on rectangle to the ball's center
    float cx = clampf(ball_pos.x, r.pos.x, r.pos.x + r.size.x);
    float cy = clampf(ball_pos.y, r.pos.y, r.pos.y + r.size.y);
    Vect closest(cx, cy);

    // Vector from closest point to ball center
    Vect d = ball_pos - closest;

    // If distance squared is greater than radius squared, no collision
    if (d.LengthSquared() > ball_radius * ball_radius) return false;

    // Compute penetration distances along each side of the rectangle
    float dxLeft = std::abs(ball_pos.x + ball_radius - r.pos.x);            
    float dxRight = std::abs(r.pos.x + r.size.x - (ball_pos.x - ball_radius)); 
    float dyTop = std::abs(ball_pos.y + ball_radius - r.pos.y);           
    float dyBottom = std::abs(r.pos.y + r.size.y - (ball_pos.y - ball_radius)); 

    // Determine the minimal penetration (smallest overlap) to resolve collision
    float minPen = std::min({ dxLeft, dxRight, dyTop, dyBottom });

    // Assign collision normal based on side with minimal penetration
    if (minPen == dxLeft) out_normal = Vect(-1, 0);    // Collided with left side
    else if (minPen == dxRight) out_normal = Vect(1, 0); // Collided with right side
    else if (minPen == dyTop) out_normal = Vect(0, -1);  // Collided with top
    else out_normal = Vect(0, 1);                        // Collided with bottom

    // Return closest hit point on rectangle
    out_hit_pos = closest;

    // Time of impact not calculated here
    out_t = 0.0f;

    return true;
}




/* ----------------- Ball Reflection & Paddle Bounce ----------------- */

void ArkanoidImpl::reflect_ball(const Vect& normal)
{
    // Reflect the ball velocity across the given normal
    Vect v = ball_vel;
    float dot = v.x * normal.x + v.y * normal.y;
    Vect reflected = v - normal * (2.0f * dot);

    // Prevent too-flat trajectories by enforcing minimum velocity components
    float min_comp = 0.15f * ball_speed_cur;
    if (std::abs(reflected.x) < min_comp)
        reflected.x = sgn(reflected.x == 0 ? ((rand() % 2) * 2 - 1) : reflected.x) * min_comp;
    if (std::abs(reflected.y) < min_comp)
        reflected.y = sgn(reflected.y == 0 ? -1.f : reflected.y) * min_comp;

    ball_vel = reflected;
}

void ArkanoidImpl::bounce_from_carriage(const Rect& r, const Vect& hit_pos_world)
{
    // Bounce off paddle with angle influenced by hit position
    float t = (hit_pos_world.x - r.pos.x) / std::max(1.0f, r.size.x); // normalized hit [0..1]
    float angle = (t - 0.5f) * 1.2f; // ~±0.6 rad (~±34°)
    Vect dir(std::sin(angle), -std::cos(angle));
    ball_vel = dir * ball_speed_cur;
}




/* ----------------- Collision Handling ----------------- */

// Main collision handler for ball with paddle and bricks
void ArkanoidImpl::handle_collisions(ArkanoidDebugData& debug_data)
{
    // ----- Paddle collision -----
    {
        Vect n, hit_pos;
        float t;

        // Check collision with paddle (carriage)
        if (collide_ball_with_rect(carriage_world, n, hit_pos, t)) {
            // Move ball just above paddle to prevent sticking
            ball_pos.y = carriage_world.pos.y - ball_radius - 0.5f;

            // Reflect ball based on where it hit the paddle
            bounce_from_carriage(carriage_world, hit_pos);

            // Record hit for debug visualization
            add_debug_hit(debug_data, hit_pos, Vect(0, -1));
        }
    }

    // ----- Brick collisions -----
    for (auto& b : bricks) {
        if (!b.alive) continue;  // Skip destroyed bricks

        Vect n, hit_pos;
        float t;

        // Check collision with current brick
        if (collide_ball_with_rect(b.rect_world, n, hit_pos, t)) {
            if (!pierce_mode) reflect_ball(n); // Reflect ball if not piercing

            if (b.hit_points > 1) {
                // ----- Partial damage brick -----
                b.hit_points -= 1;
                score += (b.score / 3) * score_mult_value;

                // Modify brick color to indicate damage visually
                int r = (b.base_color >> IM_COL32_R_SHIFT) & 255;
                int g = (b.base_color >> IM_COL32_G_SHIFT) & 255;
                int bl = (b.base_color >> IM_COL32_B_SHIFT) & 255;

                if (b.hit_points == 2) {
                    r = std::min(255, r + 30);
                    g = std::max(60, g - 20);
                    bl = std::max(30, bl - 60);
                }
                else if (b.hit_points == 1) {
                    r = std::min(255, r + 60);
                    g = std::max(40, g - 40);
                    bl = std::max(20, bl - 100);
                }

                b.color = IM_COL32(r, g, bl, 255);

                // Spawn small particles at collision for visual effect
                spawn_particles(rect_center(b.rect_world), b.color, 6);

                // Update combo counter
                combo_mult = std::min(9, combo_mult + 1);
                combo_timer = combo_window;

                add_debug_hit(debug_data, hit_pos, n);

                if (!pierce_mode) break; // Stop checking other bricks if not piercing
            }
            else {
                // ----- Destroy brick -----
                b.alive = false;
                score += b.score * combo_mult * score_mult_value;

                // Update combo
                combo_mult = std::min(9, combo_mult + 1);
                combo_timer = combo_window;

                // Spawn larger particle effect
                spawn_particles(rect_center(b.rect_world), b.color, 14);

                // Spawn bonus if brick has one
                if (b.bonus) {
                    Vect center = rect_center(b.rect_world);
                    std::mt19937 rng((uint32_t)(center.x * 1000 + center.y));
                    int choice = std::uniform_int_distribution<int>(0, 6)(rng);
                    switch (choice) {
                    case 0: spawn_bonus_at(center, BonusType::SpeedUp); break;
                    case 1: spawn_bonus_at(center, BonusType::EnlargePaddle); break;
                    case 2: spawn_bonus_at(center, BonusType::ExtraLife); break;
                    case 3: spawn_bonus_at(center, BonusType::Pierce); break;
                    case 4: spawn_bonus_at(center, BonusType::Points); break;
                    case 5: spawn_bonus_at(center, BonusType::Magnet); break;
                    case 6: spawn_bonus_at(center, BonusType::ScoreMult); break;
                    }
                }

                // Record debug hit
                add_debug_hit(debug_data, hit_pos, n);

                // Increment destroyed bricks counter
                destroyed_bricks_count++;

                // Speed up ball every N destroyed bricks
                if (destroyed_bricks_count % bricks_to_speedup == 0)
                    ball_speed_target = clampf(ball_speed_target * speedup_factor, ball_min_speed, ball_max_speed);

                if (!pierce_mode) break; // Stop checking other bricks if not piercing
            }
        }
    }
}

// Record collision information for debugging visualization
void ArkanoidImpl::add_debug_hit(ArkanoidDebugData& debug_data, const Vect& world_pos, const Vect& normal)
{
    ArkanoidDebugData::Hit h;
    // Convert world coordinates to screen coordinates for debug rendering
    h.screen_pos = Vect(world_pos.x * screen_scale.x, world_pos.y * screen_scale.y);
    h.normal = normal;
    debug_data.hits.push_back(std::move(h));
}





/* ----------------- Bonus Management ----------------- */

void ArkanoidImpl::spawn_bonus_at(const Vect& world_pos, BonusType type)
{
    Bonus b;
    float w = brick_size.x * 0.7f;
    float h = brick_size.y * 0.7f;
    b.rect_world = make_rect_xywh(world_pos.x - w * 0.5f, world_pos.y - h * 0.5f, w, h);
    b.type = type;
    b.vel = Vect(0.0f, 80.0f);
    b.alive = true;
    b.glow = 0.0f;

    // Assign color per bonus type
    switch (type) {
    case BonusType::SpeedUp: b.color = IM_COL32(255, 180, 80, 255); break;
    case BonusType::EnlargePaddle: b.color = IM_COL32(120, 200, 255, 255); break;
    case BonusType::ExtraLife: b.color = IM_COL32(200, 240, 140, 255); break;
    case BonusType::Pierce: b.color = IM_COL32(255, 120, 120, 255); break;
    case BonusType::SlowMo: b.color = IM_COL32(180, 140, 255, 255); break;
    case BonusType::Points: b.color = IM_COL32(255, 220, 120, 255); b.points = 50; break;
    case BonusType::Magnet: b.color = IM_COL32(160, 255, 200, 255); break;
    case BonusType::ScoreMult: b.color = IM_COL32(255, 160, 220, 255); break;
    default: b.color = IM_COL32(255, 255, 255, 255); break;
    }

    bonuses.push_back(std::move(b));
}

void ArkanoidImpl::integrate_bonuses(float dt)
{
    for (auto& b : bonuses) {
        if (!b.alive) continue;

        // Pulsating glow
        b.glow += dt * 6.0f;
        if (b.glow > 6.28f) b.glow -= 6.28f;

        // Magnet attraction
        if (magnet_active) {
            Vect center = rect_center(carriage_world);
            Vect dir = center - rect_center(b.rect_world);
            float dist = dir.Length();
            if (dist > 1e-4f) b.vel = b.vel + dir.Normalized() * (magnet_strength / (0.5f + dist * 0.02f)) * dt;
        }
        else {
            b.vel.y = 80.0f;
        }

        b.rect_world.pos += b.vel * dt;

        // Paddle collision
        if (b.rect_world.pos.x + b.rect_world.size.x >= carriage_world.pos.x &&
            b.rect_world.pos.x <= carriage_world.pos.x + carriage_world.size.x &&
            b.rect_world.pos.y + b.rect_world.size.y >= carriage_world.pos.y &&
            b.rect_world.pos.y <= carriage_world.pos.y + carriage_world.size.y) {
            apply_bonus(b);
            b.alive = false;
        }

        // Fell off screen
        if (b.rect_world.pos.y > world_size.y + 20.0f) b.alive = false;
    }

    // Remove inactive bonuses
    bonuses.erase(std::remove_if(bonuses.begin(), bonuses.end(), [](const Bonus& b) { return !b.alive; }), bonuses.end());
}

void ArkanoidImpl::apply_bonus(Bonus& b)
{
    // Apply bonus effect
    switch (b.type) {
    case BonusType::SpeedUp: ball_speed_target = std::min(ball_speed_target * 1.15f + 10.0f, ball_max_speed); break;
    case BonusType::EnlargePaddle: {
        float new_width = std::max(carriage_world.size.x * 1.3f, carriage_world.size.x);
        carriage_world.size.x = clampf(new_width, ArkanoidSettings::carriage_width_min, world_size.x * 0.95f);
        cheat_enlarge_paddle = false;
        clamp_carriage();
        break;
    }
    case BonusType::ExtraLife: lives++; break;
    case BonusType::Pierce: pierce_mode = true; pierce_timer = pierce_duration; break;
    case BonusType::SlowMo: slowmo_mode = true; slowmo_timer = 5.0f; ball_speed_target *= 0.4f; break;
    case BonusType::Points: score += b.points * score_mult_value; break;
    case BonusType::Magnet: magnet_active = true; magnet_timer = magnet_duration; break;
    case BonusType::ScoreMult: score_mult_active = true; score_mult_timer = score_mult_duration; score_mult_value = (rand() % 2) ? 2 : 3; break;
    default: break;
    }
}




/* ----------------- Particle System ----------------- */

// Spawn visual particles at a given world position
void ArkanoidImpl::spawn_particles(const Vect& world_pos, ImU32 color, int count)
{
    // Seed random generator based on position to get reproducible particle patterns
    std::mt19937 rng((uint32_t)(world_pos.x * 1000 + world_pos.y));

    // Random distributions for particle angle, speed, and size
    std::uniform_real_distribution<float> ang(-3.14159f, 3.14159f); // Full circle in radians
    std::uniform_real_distribution<float> spd(60.0f, 220.0f);       // Particle speed
    std::uniform_real_distribution<float> sz(1.0f, 4.0f);           // Particle size

    for (int i = 0; i < count; ++i) {
        Particle p;

        // Randomize movement direction
        float a = ang(rng);

        // Set initial position to the world position passed in
        p.pos = world_pos;

        // Velocity in the direction 'a' scaled by random speed
        p.vel = Vect(std::cos(a), std::sin(a)) * spd(rng);

        // Particle lifetime: random small variation around 0.6 seconds
        p.life = 0.6f + (rng() % 100) * 0.002f;

        // Random size for visual variation
        p.size = sz(rng);

        // Set color as passed in (usually same as brick hit)
        p.color = color;

        // Add particle to global particle list for rendering/updating
        particles.push_back(std::move(p));
    }
}


void ArkanoidImpl::integrate_particles(float dt)
{
    for (auto& p : particles) {
        p.pos += p.vel * dt;
        p.vel.y += 200.0f * dt;       // gravity effect
        p.vel *= (1.0f - 2.0f * dt);  // damping
        p.life -= dt;
    }

    particles.erase(std::remove_if(particles.begin(), particles.end(), [](const Particle& p) { return p.life <= 0.0f; }), particles.end());
}

void ArkanoidImpl::draw_particles(ImDrawList& dl)
{
    for (const auto& p : particles) {
        ImVec2 pos(p.pos.x * screen_scale.x, p.pos.y * screen_scale.y);
        float s = p.size * screen_scale.x;
        float alpha = clampf(p.life / 0.8f, 0.0f, 1.0f);
        ImU32 col = ImColor(
            (int)((p.color >> IM_COL32_R_SHIFT) & 255),
            (int)((p.color >> IM_COL32_G_SHIFT) & 255),
            (int)((p.color >> IM_COL32_B_SHIFT) & 255),
            (int)(255.0f * alpha)
        );
        dl.AddCircleFilled(pos, s, col, 8);
    }
}




/* ----------------- Drawing World & UI ----------------- */

void ArkanoidImpl::draw_world(ImDrawList& dl)
{
    // Draw particles under everything
    draw_particles(dl);

    // Draw bricks
    for (const auto& b : bricks) {
        if (!b.alive) continue;

        ImVec2 p0 = ImVec2(b.rect_world.pos.x * screen_scale.x, b.rect_world.pos.y * screen_scale.y);
        ImVec2 p1 = ImVec2((b.rect_world.pos.x + b.rect_world.size.x) * screen_scale.x,
            (b.rect_world.pos.y + b.rect_world.size.y) * screen_scale.y);
        float rounding = (b.hit_points >= 3) ? 8.0f : 6.0f;

        // Base brick
        dl.AddRectFilled(p0, p1, b.color, rounding);
        dl.AddRect(p0, p1, IM_COL32(0, 0, 0, 80), rounding);

        // Top highlight
        ImVec2 t0 = p0;
        ImVec2 t1 = ImVec2(p1.x, p0.y + (p1.y - p0.y) * 0.18f);
        dl.AddRectFilled(t0, t1, IM_COL32(255, 255, 255, 20), rounding);

        // HP marker
        if (b.hit_points > 1) {
            char buf[8];
            snprintf(buf, sizeof(buf), "x%d", b.hit_points);
            dl.AddText(ImVec2(p0.x + 6, p0.y + 6), IM_COL32(30, 30, 30, 200), buf);
        }
    }

    // Draw bonuses and paddle
    draw_bonuses(dl);

    ImVec2 p0 = ImVec2(carriage_world.pos.x * screen_scale.x, carriage_world.pos.y * screen_scale.y);
    ImVec2 p1 = ImVec2((carriage_world.pos.x + carriage_world.size.x) * screen_scale.x,
        (carriage_world.pos.y + carriage_world.size.y) * screen_scale.y);
    dl.AddRectFilled(p0, p1, IM_COL32(200, 230, 255, 255), 8.0f);
    dl.AddRect(p0, p1, IM_COL32(0, 0, 0, 120), 8.0f);

    // Paddle highlight
    ImVec2 c0 = ImVec2((p0.x + p1.x) * 0.4f, p0.y);
    ImVec2 c1 = ImVec2((p0.x + p1.x) * 0.6f, p1.y);
    dl.AddRectFilled(c0, c1, IM_COL32(255, 255, 255, 30), 6.0f);

    // Magnet indicator
    if (magnet_active) {
        ImVec2 center = ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
        float radius = (p1.x - p0.x) * 0.9f;
        dl.AddCircle(center, radius, IM_COL32(160, 255, 200, 90), 48, 2.5f);
    }

    // Ball trail
    if (trail_mode && !ball_trail.empty()) {
        float alpha = 40.0f;
        for (int i = 0; i < (int)ball_trail.size(); ++i) {
            ImVec2 sp = ImVec2(ball_trail[i].x * screen_scale.x, ball_trail[i].y * screen_scale.y);
            float sr = ball_radius * screen_scale.x * (0.6f * (1.0f - float(i) / ball_trail.size()) + 0.2f);
            ImU32 col = IM_COL32(120, 70, 100, int(alpha * (1.0f - float(i) / ball_trail.size())));
            dl.AddCircleFilled(sp, sr, col, 16);
        }
    }

    // Ball
    ImVec2 sp = ImVec2(ball_pos.x * screen_scale.x, ball_pos.y * screen_scale.y);
    float sr = ball_radius * screen_scale.x;
    ImU32 col = pierce_mode ? IM_COL32(255, 120, 120, 255) : IM_COL32(220, 70, 170, 255);
    dl.AddCircleFilled(sp, sr, col, 32);
    dl.AddCircle(sp, sr, IM_COL32(0, 0, 0, 130), 32, 1.5f);

    if (cheat_freeze_ball)
        dl.AddCircle(sp, sr + 6.0f, IM_COL32(180, 220, 255, 80), 32, 3.0f);
}

// ----------------- Draw Bonuses -----------------
void ArkanoidImpl::draw_bonuses(ImDrawList& dl)
{
    for (const auto& b : bonuses) {
        // Convert world coordinates to screen coordinates
        ImVec2 p0 = ImVec2(b.rect_world.pos.x * screen_scale.x, b.rect_world.pos.y * screen_scale.y);
        ImVec2 p1 = ImVec2((b.rect_world.pos.x + b.rect_world.size.x) * screen_scale.x,
            (b.rect_world.pos.y + b.rect_world.size.y) * screen_scale.y);

        // Draw glow around the bonus with a pulsing alpha
        float pulse = 0.5f + 0.5f * std::sin(b.glow);
        ImU32 glowcol = IM_COL32(
            (b.color >> IM_COL32_R_SHIFT) & 255,
            (b.color >> IM_COL32_G_SHIFT) & 255,
            (b.color >> IM_COL32_B_SHIFT) & 255,
            int(80.0f * pulse)
        );
        dl.AddRectFilled(ImVec2(p0.x - 3.0f, p0.y - 3.0f), ImVec2(p1.x + 3.0f, p1.y + 3.0f), glowcol, 8.0f);

        // Draw bonus rectangle
        dl.AddRectFilled(p0, p1, b.color, 6.0f);
        dl.AddRect(p0, p1, IM_COL32(0, 0, 0, 100), 6.0f);

        // Draw label representing the bonus type
        const char* label = "?";
        switch (b.type) {
        case BonusType::SpeedUp:        label = "S"; break;
        case BonusType::EnlargePaddle:  label = "P"; break;
        case BonusType::ExtraLife:      label = "L"; break;
        case BonusType::Pierce:         label = "X"; break;
        case BonusType::SlowMo:         label = "Z"; break;
        case BonusType::Points:         label = "+"; break;
        case BonusType::Magnet:         label = "M"; break;
        case BonusType::ScoreMult:      label = "★"; break;
        default: break;
        }

        // Center text in the bonus rectangle
        ImVec2 mid = ImVec2((p0.x + p1.x) * 0.5f - 6.0f, (p0.y + p1.y) * 0.5f - 6.0f);
        dl.AddText(mid, IM_COL32(24, 24, 24, 240), label);
    }
}


// ----------------- Draw Game UI -----------------
void ArkanoidImpl::draw_ui(ImGuiIO& io, ImDrawList& dl)
{
    // Panel background rectangle
    ImU32 bg = IM_COL32(12, 12, 12, 160);
    ImU32 white = IM_COL32(240, 240, 240, 255);
    ImVec2 tl(12, 12); // top-left
    ImVec2 br(tl.x + 360, tl.y + 120); // bottom-right
    dl.AddRectFilled(tl, br, bg, 8.0f);
    dl.AddRect(tl, br, IM_COL32(255, 255, 255, 10), 8.0f);

    // Draw Score and Lives
    dl.AddText(ImVec2(tl.x + 12, tl.y + 8), white, ("Score: " + std::to_string(score)).c_str());
    dl.AddText(ImVec2(tl.x + 12, tl.y + 26), white, ("Lives: " + std::to_string(lives)).c_str());

    // Draw Ball Speed Bar
    float bar_x = tl.x + 12, bar_w = 360 - 24, bar_y = tl.y + 46;
    dl.AddRectFilled(ImVec2(bar_x, bar_y), ImVec2(bar_x + bar_w, bar_y + 12), IM_COL32(60, 60, 60, 180), 6.0f);
    float fill_w = clampf((ball_speed_cur / std::max(1.0f, ball_speed_target)) * bar_w * 0.9f, 0.0f, bar_w);
    dl.AddRectFilled(ImVec2(bar_x + 2, bar_y + 2), ImVec2(bar_x + 2 + fill_w, bar_y + 10), IM_COL32(120, 200, 255, 220), 5.0f);
    char buf[64];
    snprintf(buf, sizeof(buf), "Speed: %.0f", ball_speed_cur);
    dl.AddText(ImVec2(bar_x + bar_w - 80, bar_y + 14), IM_COL32(220, 220, 220, 200), buf);

    // Draw Money / Balance
    std::ostringstream money_ss;
    money_ss << "Money: $" << balance << " / Total: $" << total_money;
    dl.AddText(ImVec2(tl.x + 12, tl.y + 74), IM_COL32(220, 220, 220, 220), money_ss.str().c_str());

    // Draw Active Effects / Powerups
    float icons_x = tl.x + 12, icons_y = tl.y + 120 - 20;
    std::string icons_line;
    if (magnet_active)        icons_line += "[MAGNET] ";
    if (score_mult_active)    icons_line += (score_mult_value == 2 ? "[x2 SCORE] " : "[x3 SCORE] ");
    if (pierce_mode)          icons_line += "[PIERCE] ";
    if (slowmo_mode)          icons_line += "[SLOW] ";
    if (trail_mode)           icons_line += "[TRAIL] ";
    if (cheat_invincible)     icons_line += "[GOD] ";
    if (cheat_freeze_ball)    icons_line += "[FREEZE] ";
    if (cheat_enlarge_paddle) icons_line += "[BIG PAD] ";

    // Display powerups, or a default message if none are active
    dl.AddText(ImVec2(icons_x, icons_y), !icons_line.empty() ? IM_COL32(255, 255, 255, 255) : IM_COL32(160, 160, 160, 140),
        !icons_line.empty() ? icons_line.c_str() : "No active powerups");

    // Display temporary shop message
    if (!shop_message.empty())
        dl.AddText(ImVec2(tl.x + 360 - 180, tl.y + 120 - 20), IM_COL32(200, 200, 140, 220), shop_message.c_str());

    // Draw cheats / shop panel
    draw_cheats_panel(io);
}


/* ----------------- Cheats Panel ----------------- */
void ArkanoidImpl::draw_cheats_panel(ImGuiIO& io)
{
    ImVec2 pos(io.DisplaySize.x - 260.0f, 8.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);

    ImGui::Begin("##cheats_drop", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);

    // Main popup button
    if (ImGui::Button("Cheat Shop ▾")) ImGui::OpenPopup("cheat_shop_popup");

    if (ImGui::BeginPopup("cheat_shop_popup"))
    {
        ImGui::TextColored(ImVec4(0.9f, 0.9f, 1.0f, 1.0f), "Cheat Shop");
        ImGui::Separator();

        // Display balance
        ImGui::Text("Balance: $%d  Total: $%d", balance, total_money);
        ImGui::Separator();

        // Purchase buttons
        auto try_buy = [&](int cost, auto effect, const char* msg) {
            if (ImGui::Button(msg)) {
                if (try_purchase(cost)) effect();
            }
        };

        try_buy(10, [&]() { cheat_freeze_ball = true; shop_message = "Purchased Freeze Ball!"; shop_message_timer = shop_message_duration; },
            "Buy Freeze (Q) - $10");
        try_buy(20, [&]() { lives++; shop_message = "Purchased +1 Life!"; shop_message_timer = shop_message_duration; },
            "Buy +1 Life (E) - $20");
        try_buy(10, [&]() { magnet_active = true; magnet_timer = magnet_duration; shop_message = "Purchased Magnet!"; shop_message_timer = shop_message_duration; },
            "Buy Magnet (X) - $10");
        try_buy(15, [&]() { score_mult_active = true; score_mult_timer = score_mult_duration; score_mult_value = (rand() % 2) ? 2 : 3; shop_message = "Purchased Score Multiplier!"; shop_message_timer = shop_message_duration; },
            "Buy Multiplier (T) - $15");
        try_buy(60, [&]() { cheat_invincible = true; shop_message = "Purchased Invincibility!"; shop_message_timer = shop_message_duration; },
            "Buy Invincibility (Y) - $60");

        ImGui::Separator();
        ImGui::TextWrapped("Secret cheats and more");
        ImGui::TextWrapped("Hotkeys: A/D - move, 1/2/3 - speed presets, C - pierce, R - restart, N - delete row of blocks");

        ImGui::EndPopup();
    }

    ImGui::SameLine();
    ImGui::TextDisabled("(Shop)");
    ImGui::End();
}


/* ----------------- Main Debug Menu ----------------- */
void ArkanoidImpl::draw_main_debug_menu(ImGuiIO& io)
{
    float w = 560.0f;
    ImVec2 pos((io.DisplaySize.x - w) * 0.5f, 6.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);

    ImGui::Begin("##ark_debug_top", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);

    ImGui::SameLine((w * 0.5f) - 80.0f);
    if (ImGui::Button("Arkanoid (Debug) ▾")) ImGui::OpenPopup("ark_debug_popup");

    if (ImGui::BeginPopup("ark_debug_popup"))
    {
        ImGui::TextColored(ImVec4(0.85f, 0.92f, 1.0f, 1.0f), "Arkanoid (Debug) — Test / Tweak");
        ImGui::Separator();

        // Controls
        if (ImGui::Button(paused ? "Resume" : "Pause")) paused = !paused;
        ImGui::SameLine();
        if (ImGui::Button("Reset")) reset(settings);
        ImGui::SameLine();
        if (ImGui::Button("Rebuild Level")) build_level(settings);
        ImGui::Separator();

        // Level parameters
        bool rebuild = false;
        int cols = settings.bricks_columns_count;
        int rows = settings.bricks_rows_count;
        float padx = settings.bricks_columns_padding;
        float pady = settings.bricks_rows_padding;

        if (ImGui::SliderInt("Columns", &cols, ArkanoidSettings::bricks_columns_min, ArkanoidSettings::bricks_columns_max)) {
            settings.bricks_columns_count = cols; rebuild = true;
        }
        if (ImGui::SliderInt("Rows", &rows, ArkanoidSettings::bricks_rows_min, ArkanoidSettings::bricks_rows_max)) {
            settings.bricks_rows_count = rows; rebuild = true;
        }
        if (ImGui::SliderFloat("Pad X", &padx, ArkanoidSettings::bricks_columns_padding_min, ArkanoidSettings::bricks_columns_padding_max)) {
            settings.bricks_columns_padding = padx; rebuild = true;
        }
        if (ImGui::SliderFloat("Pad Y", &pady, ArkanoidSettings::bricks_rows_padding_min, ArkanoidSettings::bricks_rows_padding_max)) {
            settings.bricks_rows_padding = pady; rebuild = true;
        }
        if (rebuild) build_level(settings);

        ImGui::Separator();

        // Ball & paddle tweaking
        float bradius = ball_radius;
        if (ImGui::SliderFloat("Ball Radius", &bradius, 4.0f, 48.0f)) {
            ball_radius = bradius;
            ball_pos.y = carriage_world.pos.y - ball_radius - 1.0f;
        }
        float pwidth = carriage_world.size.x;
        if (ImGui::SliderFloat("Paddle Width", &pwidth, 40.0f, world_size.x * 0.9f)) {
            carriage_world.size.x = pwidth;
            clamp_carriage();
        }

        ImGui::Separator();

        // Physics debug
        ImGui::SliderFloat("Ball target speed", &ball_speed_target, ball_min_speed, ball_max_speed);
        ImGui::Checkbox("Show Trail", &trail_mode);

        ImGui::Separator();

        // Debug info
        ImGui::Text("Destroyed bricks: %d", destroyed_bricks_count);
        ImGui::Text("Next speedup in: %d", bricks_to_speedup - (destroyed_bricks_count % bricks_to_speedup));

        ImGui::EndPopup();
    }

    ImGui::End();
}


/* ----------------- Centered Modal ----------------- */
void ArkanoidImpl::draw_centered_modal(ImGuiIO& io, ImDrawList& dl, const char* title, const char* msg, ImU32 color)
{
    ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    ImVec2 size(420.0f, 140.0f);
    ImVec2 tl(center.x - size.x * 0.5f, center.y - size.y * 0.5f);
    ImVec2 br(center.x + size.x * 0.5f, center.y + size.y * 0.5f);

    dl.AddRectFilled(tl, br, IM_COL32(20, 20, 20, 220), 12.0f);
    dl.AddRect(tl, br, IM_COL32(255, 255, 255, 30), 12.0f);
    dl.AddRectFilled(ImVec2(tl.x + 6, tl.y + 6), ImVec2(br.x + 6, br.y + 6), IM_COL32(0, 0, 0, 50), 12.0f);

    dl.AddText(ImVec2(center.x - 10.0f * (float)strlen(title) * 0.5f + 8, tl.y + 14), color, title);
    dl.AddText(ImVec2(tl.x + 20, tl.y + 56), IM_COL32(220, 220, 220, 240), msg);
}
