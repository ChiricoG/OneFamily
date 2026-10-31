#include "ps_background.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Fast branchless pixel alpha blend using fixed point (>> 8)
static inline void blend_pixel_fast(uint32_t* fb, int idx, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (a == 0) return;
    uint32_t bg = fb[idx];
    uint32_t bgR = (bg >> 16) & 0xFF;
    uint32_t bgG = (bg >> 8) & 0xFF;
    uint32_t bgB = bg & 0xFF;

    uint32_t invA = 256 - a;
    uint32_t outR = (r * a + bgR * invA) >> 8;
    uint32_t outG = (g * a + bgG * invA) >> 8;
    uint32_t outB = (b * a + bgB * invA) >> 8;

    fb[idx] = 0x80000000 | (outR << 16) | (outG << 8) | outB;
}

PSBackground::PSBackground() : m_enabled(true), m_time(0.0f) {
}

PSBackground::~PSBackground() {
}

void PSBackground::init() {
    m_time = 0.0f;
    m_enabled = true;

    // Initialize particles with randomized distribution
    for (int i = 0; i < PARTICLE_COUNT; i++) {
        m_particles[i].baseX = (float)(rand() % 1920);
        m_particles[i].y = (float)(rand() % 1080);
        m_particles[i].speedY = 15.0f + (float)(rand() % 25);         // 15 to 40 px/s
        m_particles[i].wobblePhase = (float)(rand() % 628) / 100.0f; // 0 to 2*PI
        m_particles[i].wobbleSpeed = 0.5f + (float)(rand() % 100) / 100.0f; // 0.5 to 1.5 rad/s
        m_particles[i].wobbleAmp = 10.0f + (float)(rand() % 25);     // 10 to 35 px amplitude
        m_particles[i].radius = 3.5f + (float)(rand() % 6);          // 3.5 to 9.5 px
        m_particles[i].maxAlpha = 40.0f + (float)(rand() % 70);      // 40 to 110 max alpha
    }
}

void PSBackground::update(float dt) {
    if (!m_enabled) return;

    m_time += dt;
    if (m_time > 100000.0f) m_time = 0.0f; // Prevent floating point precision loss

    for (int i = 0; i < PARTICLE_COUNT; i++) {
        // Move upward
        m_particles[i].y -= m_particles[i].speedY * dt;
        m_particles[i].wobblePhase += m_particles[i].wobbleSpeed * dt;

        // Wrap around when particle floats above screen
        if (m_particles[i].y < -20.0f) {
            m_particles[i].y = 1100.0f + (float)(rand() % 60);
            m_particles[i].baseX = (float)(rand() % 1920);
            m_particles[i].speedY = 15.0f + (float)(rand() % 25);
            m_particles[i].radius = 3.5f + (float)(rand() % 6);
            m_particles[i].maxAlpha = 40.0f + (float)(rand() % 70);
        }
    }
}

void PSBackground::render_ribbon(uint32_t* fb, int width, int height,
                                 float baseY, float amp1, float amp2,
                                 float freq1, float freq2, float speed1, float speed2,
                                 uint8_t r, uint8_t g, uint8_t b, uint8_t maxAlpha, int thickness) {
    if (!fb || thickness <= 0) return;

    float invThicknessSq = 1.0f / (float)(thickness * thickness);

    for (int x = 0; x < width; x++) {
        // Compute undulating center of the wave ribbon
        float xf = (float)x;
        float cy = baseY + amp1 * sinf(xf * freq1 + m_time * speed1)
                         + amp2 * sinf(xf * freq2 - m_time * speed2);

        int startY = (int)(cy - (float)thickness);
        int endY   = (int)(cy + (float)thickness);

        if (startY < 0) startY = 0;
        if (endY >= height) endY = height - 1;

        for (int y = startY; y <= endY; y++) {
            float dy = (float)y - cy;
            float distSq = dy * dy;
            float factor = 1.0f - distSq * invThicknessSq;
            if (factor <= 0.0f) continue;

            // Smooth cosine bell-curve falloff
            uint8_t a = (uint8_t)((float)maxAlpha * factor * factor);
            blend_pixel_fast(fb, y * width + x, r, g, b, a);
        }
    }
}

void PSBackground::render_particles(uint32_t* fb, int width, int height) {
    if (!fb) return;

    for (int i = 0; i < PARTICLE_COUNT; i++) {
        float xf = m_particles[i].baseX + m_particles[i].wobbleAmp * sinf(m_particles[i].wobblePhase);
        float yf = m_particles[i].y;

        if (xf < -20.0f || xf > (float)(width + 20) || yf < -20.0f || yf > (float)(height + 20)) {
            continue;
        }

        // Calculate opacity based on vertical position (fade-in near bottom, fade-out near top)
        float fade = 1.0f;
        if (yf > 900.0f) {
            fade = (1080.0f - yf) / 180.0f;
            if (fade < 0.0f) fade = 0.0f;
        } else if (yf < 250.0f) {
            fade = yf / 250.0f;
            if (fade < 0.0f) fade = 0.0f;
        }

        float effectiveMaxAlpha = m_particles[i].maxAlpha * fade;
        if (effectiveMaxAlpha <= 1.0f) continue;

        int rad = (int)(m_particles[i].radius + 0.5f);
        if (rad < 1) rad = 1;
        float invRadSq = 1.0f / (m_particles[i].radius * m_particles[i].radius);

        int minX = (int)(xf - rad);
        int maxX = (int)(xf + rad);
        int minY = (int)(yf - rad);
        int maxY = (int)(yf + rad);

        if (minX < 0) minX = 0;
        if (maxX >= width) maxX = width - 1;
        if (minY < 0) minY = 0;
        if (maxY >= height) maxY = height - 1;

        // PlayStation Sparkle Tint: Soft glowing Cyan-White
        const uint8_t pr = 195;
        const uint8_t pg = 225;
        const uint8_t pb = 255;

        for (int py = minY; py <= maxY; py++) {
            float dy = (float)py - yf;
            for (int px = minX; px <= maxX; px++) {
                float dx = (float)px - xf;
                float d2 = dx * dx + dy * dy;
                float falloff = 1.0f - d2 * invRadSq;
                if (falloff > 0.0f) {
                    uint8_t a = (uint8_t)(effectiveMaxAlpha * falloff * falloff);
                    blend_pixel_fast(fb, py * width + px, pr, pg, pb, a);
                }
            }
        }
    }
}

void PSBackground::render(Scene2D* scene, const uint32_t* baseWallpaper) {
    if (!scene) return;

    // 1. Copy base wallpaper to direct framebuffer
    if (baseWallpaper) {
        scene->CopyToActiveBuffer(baseWallpaper);
    } else {
        scene->DrawRectangle(0, 0, scene->GetWidth(), scene->GetHeight(), Color{ 10, 24, 45 });
    }

    if (!m_enabled) return;

    uint32_t* fb = scene->GetActiveFrameBuffer();
    if (!fb) return;

    int w = scene->GetWidth();
    int h = scene->GetHeight();

    // 2. Render XMB Ribbon Waves
    // Wave 1: Primary Vibrant Cyan-Blue Wave
    render_ribbon(fb, w, h,
                  650.0f,               // baseY
                  55.0f, 30.0f,         // amp1, amp2
                  0.00314f, 0.00628f,   // freq1, freq2
                  0.75f, 1.20f,         // speed1, speed2
                  35, 145, 255,         // PS Bright Blue / Cyan
                  85,                   // maxAlpha
                  36);                  // thickness

    // Wave 2: Secondary Deep Royal Blue Wave (intertwining counter-motion)
    render_ribbon(fb, w, h,
                  710.0f,               // baseY
                  45.0f, 35.0f,         // amp1, amp2
                  0.00260f, 0.00520f,   // freq1, freq2
                  0.55f, 0.95f,         // speed1, speed2
                  0, 85, 215,           // PS Deep Royal Blue
                  75,                   // maxAlpha
                  44);                  // thickness

    // Wave 3: Subtle High-Altitude Electric Glow
    render_ribbon(fb, w, h,
                  540.0f,               // baseY
                  35.0f, 20.0f,         // amp1, amp2
                  0.00380f, 0.00750f,   // freq1, freq2
                  0.40f, 0.80f,         // speed1, speed2
                  65, 180, 255,         // Soft sky cyan
                  40,                   // maxAlpha
                  28);                  // thickness

    // 3. Render Floating Light Particles / Bokeh Sparkles
    render_particles(fb, w, h);
}
