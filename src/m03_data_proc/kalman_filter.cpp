#include "qdgz300/m03_data_proc/kalman_filter.h"

#include <cmath>

namespace qdgz300::m03
{
    void KalmanFilter::init(double x, double y, double z)
    {
        initialized_ = true;
        x_ = x;
        y_ = y;
        z_ = z;
        vx_ = 0.0;
        vy_ = 0.0;
        vz_ = 0.0;
    }

    void KalmanFilter::predict(double dt_sec)
    {
        if (!initialized_)
        {
            return;
        }
        x_ += vx_ * dt_sec;
        y_ += vy_ * dt_sec;
        z_ += vz_ * dt_sec;
    }

    void KalmanFilter::update(double obs_x, double obs_y, double obs_z)
    {
        if (!initialized_)
        {
            init(obs_x, obs_y, obs_z);
            return;
        }

        constexpr double position_gain = 0.6;
        constexpr double velocity_gain = 0.4;

        const double dx = obs_x - x_;
        const double dy = obs_y - y_;
        const double dz = obs_z - z_;

        x_ += position_gain * dx;
        y_ += position_gain * dy;
        z_ += position_gain * dz;

        vx_ += velocity_gain * dx;
        vy_ += velocity_gain * dy;
        vz_ += velocity_gain * dz;
    }

    double KalmanFilter::heading_deg() const noexcept
    {
        return std::atan2(vy_, vx_) * 180.0 / 3.14159265358979323846;
    }
}
