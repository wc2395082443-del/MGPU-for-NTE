#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace mgpu::game_work
{
class trace_log_parts
{
    std::vector<std::string> lines_;
    unsigned next_=0;
public:
    static constexpr unsigned record_bytes=32768;
    bool append(const char *line,unsigned length,const char *prefix,std::uint64_t session,std::uint64_t job)
    {
        if (length>=record_bytes) return false;
        if (length<1900) { lines_.emplace_back(line,length); return true; }
        const unsigned parts=(length+1299)/1300;
        if (parts>32) return false;
        for (unsigned part=0;part<parts;++part)
        {
            char fragment[1500];
            const auto bytes=std::min(1300u,length-part*1300);
            const int written=std::snprintf(fragment,sizeof fragment,
                "%s session=%llu job=%llu part=%u parts=%u payload=%.*s",prefix,
                static_cast<unsigned long long>(session),static_cast<unsigned long long>(job),part,parts,int(bytes),line+part*1300);
            if (written<0 || unsigned(written)>=sizeof fragment) return false;
            lines_.emplace_back(fragment,unsigned(written));
        }
        return true;
    }
    template<typename Emit> unsigned drain(unsigned limit,Emit emit)
    {
        unsigned count=0;
        while (next_<lines_.size() && count<limit) { emit(lines_[next_++].c_str()); ++count; }
        if (next_==lines_.size()) clear();
        return count;
    }
    bool empty() const { return lines_.empty(); }
    void clear() { std::vector<std::string>().swap(lines_); next_=0; }
};
}
