#pragma once

namespace mgpu::nr_preset
{
inline constexpr const char *parameter = "DLSSNR.Hint.Render.Preset";

inline bool valid(int value) { return value >= 0 && value <= 4; }

struct selection
{
    int requested = 0;
    int active = -1;
    bool loaded = false;

    bool request(int value)
    {
        if (!valid(value)) return false;
        requested = value;
        loaded = true;
        return true;
    }

    void load(int value)
    {
        request(valid(value) ? value : 0);
    }

    void begin_create() { active = requested; }

    bool pending() const { return active >= 0 && requested != active; }

    template <typename Parameters>
    void bind(Parameters *parameters) const
    {
        if (active > 0 && valid(active))
            parameters->Set(parameter, static_cast<unsigned>(active));
    }
};
}
