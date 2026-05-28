#include "control_math.h"

#include <algorithm>
#include <cmath>

namespace svg_mb_control {

double SmoothStep(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * t * ((6.0 * t - 15.0) * t + 10.0);
}

double SmoothScale(double value, double start, double full) {
    if (std::isnan(value) || full <= start) {
        return 0.0;
    }
    return SmoothStep((value - start) / (full - start));
}

double MoveTowardRateLimited(double current,
                             double target,
                             double dt_minutes,
                             double rise_per_min,
                             double fall_per_min) {
    current = std::isnan(current) ? 0.0 : current;
    const double delta = target - current;
    if (std::abs(delta) <= 0.0001 || dt_minutes <= 0.0) {
        return target;
    }

    const double rate = delta > 0.0 ? rise_per_min : fall_per_min;
    if (std::isnan(rate) || rate <= 0.0) {
        return target;
    }

    const double allowed = rate * dt_minutes;
    if (std::abs(delta) <= allowed) {
        return target;
    }
    return current + (delta > 0.0 ? allowed : -allowed);
}

}  // namespace svg_mb_control
