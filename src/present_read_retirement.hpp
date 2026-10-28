#pragma once
#include <cstdint>
#include <limits>

namespace mgpu::present_read
{
    enum class status { retired, pending, unsafe };

    class retirement
    {
        std::uint64_t value_ = 0;
        bool unfenced_ = false;

    public:
        void submitted(std::uint64_t value, bool signaled)
        {
            if (!signaled || value == 0)
                unfenced_ = true;
            else if (value > value_)
                value_ = value;
        }

        std::uint64_t value() const { return value_; }

        status check(std::uint64_t completed) const
        {
            if (unfenced_) return status::unsafe;
            if (value_ == 0) return status::retired;
            if (completed == (std::numeric_limits<std::uint64_t>::max)())
                return status::unsafe;
            return completed >= value_ ? status::retired : status::pending;
        }
    };
}
