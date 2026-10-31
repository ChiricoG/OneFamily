#pragma once

#include "common/graphics.h"
#include <stdint.h>

struct PSParticle {
    float baseX;
    float y;
    float speedY;
    float wobblePhase;
    float wobbleSpeed;
    float wobbleAmp;
    float radius;
    float maxAlpha;
};

class PSBackground {
public:
    PSBackground();
    ~PSBackground();

    void init();
    void update(float dt);
    void render(Scene2D* scene, const uint32_t* baseWallpaper);

    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }

private:
    bool m_enabled;
    float m_time;

    static const int PARTICLE_COUNT = 36;
    PSParticle m_particles[PARTICLE_COUNT];

    void render_ribbon(uint32_t* fb, int width, int height,
                       float baseY, float amp1, float amp2,
                       float freq1, float freq2, float speed1, float speed2,
                       uint8_t r, uint8_t g, uint8_t b, uint8_t maxAlpha, int thickness);

    void render_particles(uint32_t* fb, int width, int height);
};
