#pragma once
#include <cstdint>

namespace mgpu::game_work
{
enum class lifecycle { recording, closed, submitted, retired, cancelled, invalid };
struct record_state
{
    lifecycle state = lifecycle::recording;
    bool replayed = false;
    bool close()
    {
        if (state != lifecycle::recording) return false;
        state = lifecycle::closed;
        return true;
    }
    bool submit()
    {
        if (state == lifecycle::submitted || state == lifecycle::retired)
        {
            replayed = true;
            state = lifecycle::invalid;
            return false;
        }
        if (state != lifecycle::closed) return false;
        state = lifecycle::submitted;
        return true;
    }
    void reset()
    {
        if (state == lifecycle::recording || state == lifecycle::closed) state = lifecycle::cancelled;
    }
    bool retire(bool completed)
    {
        if (!completed || state != lifecycle::submitted || replayed) return false;
        state = lifecycle::retired;
        return true;
    }
};
}
