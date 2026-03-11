#pragma once

#include "qdgz300/common/types.h"

namespace qdgz300::m03
{
    class KalmanFilter
    {
    public:
        void init(double x, double y, double z);
        void predict(double dt_sec);
        void update(double obs_x, double obs_y, double obs_z);

        double x() const noexcept { return x_; }
        double y() const noexcept { return y_; }
        double z() const noexcept { return z_; }
        double vx() const noexcept { return vx_; }
        double vy() const noexcept { return vy_; }
        double vz() const noexcept { return vz_; }
        double heading_deg() const noexcept;

    private:
        bool initialized_{false};
        double x_{0.0};
        double y_{0.0};
        double z_{0.0};
        double vx_{0.0};
        double vy_{0.0};
        double vz_{0.0};
    };
}
