#pragma once
#include <array>
#include <cmath>

namespace mgpu::native_work
{
constexpr unsigned operation_limit=160;
constexpr unsigned timestamp_limit=384;
static_assert(operation_limit*2+64<=timestamp_limit);
constexpr unsigned general_job_limit=1024;
constexpr unsigned meta_job_limit=16;
constexpr unsigned job_limit=general_job_limit+meta_job_limit;
struct selection_policy
{
    std::array<unsigned,4> origins{};
    unsigned selected=0,general_selected=0,meta_selected=0;
    bool learn(unsigned origin,unsigned type,unsigned marks,bool full,bool scoped,double milliseconds,bool wholly_unclassified=false)
    {
        if (!origin || type!=0 || marks<2 || (marks!=2 && !wholly_unclassified) || !full || scoped || !std::isfinite(milliseconds) || milliseconds<2.0) return false;
        for (auto value:origins) if (value==origin) return false;
        for (auto &value:origins) if (!value) { value=origin; return true; }
        return false;
    }
    bool candidate(unsigned origin) const
    {
        if (!origin || selected>=job_limit) return false;
        for (auto value:origins) if (value==origin) return true;
        return false;
    }
    bool claim(bool meta)
    {
        if (selected>=job_limit || (meta ? meta_selected>=meta_job_limit : general_selected>=general_job_limit)) return false;
        ++selected;
        if (meta) ++meta_selected; else ++general_selected;
        return true;
    }
};
}
