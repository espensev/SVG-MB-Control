#pragma once

namespace svg_mb_control {

// Quintic smootherstep, clamping input to [0, 1] before evaluation.
double SmoothStep(double t);

// Maps `value` to [0, 1] via SmoothStep across [start, full]. Returns 0
// when value is NaN or when full <= start (band collapsed / inert).
double SmoothScale(double value, double start, double full);

// Rate-limited step from `current` toward `target`. Used by the cadence
// integrator (asymmetric rise/fall) and by the low-band stage-boost
// integrator. NaN `current` is treated as 0.0; non-positive `dt_minutes`
// snaps to `target`; NaN or non-positive direction-rate snaps to
// `target` (no rate limit applied).
double MoveTowardRateLimited(double current,
                             double target,
                             double dt_minutes,
                             double rise_per_min,
                             double fall_per_min);

}  // namespace svg_mb_control
