#pragma once
#include "../types.h"

namespace dsp::math {
    // Fast sin/cos pair for x in [-pi, pi] - the range a phase control loop maintains.
    // libm sinf/cosf per sample dominates a sample-rate PLL (two calls per sample);
    // this folds into [-pi/2, pi/2] and evaluates least-squares polynomials instead.
    // Max error: sin 1.0e-8, cos 1.4e-7 - orders of magnitude below the loop's own
    // phase noise. Coefficients fitted over [-pi/2, pi/2] (odd deg-9 / even deg-8).
    inline complex_t fastPhasor(float x) {
        float sign = 1.0f;
        if (x > 1.57079632679f) {
            x -= 3.14159265359f;
            sign = -1.0f;
        }
        else if (x < -1.57079632679f) {
            x += 3.14159265359f;
            sign = -1.0f;
        }
        const float x2 = x * x;
        const float s = x * (9.9999998277e-01f + x2 * (-1.6666651514e-01f + x2 * (8.3329639090e-03f + x2 * (-1.9804748135e-04f + x2 * 2.5980951129e-06f))));
        const float c = 9.9999996725e-01f + x2 * (-4.9999926870e-01f + x2 * (4.1664090612e-02f + x2 * (-1.3857415779e-03f + x2 * 2.3237497012e-05f)));
        return { sign * c, sign * s };
    }
}
