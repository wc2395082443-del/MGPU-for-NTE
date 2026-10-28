#include "../src/trace_log_parts.hpp"
#include <cstdio>
#include <string>
#include <vector>

int main()
{
    mgpu::game_work::trace_log_parts output;
    const std::string large(22000,'a');
    if (!output.append(large.c_str(),unsigned(large.size()),"[MGPU][M339] kind=fine_part",7,9) || output.empty()) return 1;
    std::vector<std::string> lines;
    unsigned polls=0;
    while (!output.empty())
    {
        const auto count=output.drain(8,[&](const char *line) { lines.emplace_back(line); });
        if (!count || count>8 || ++polls>8) return 2;
    }
    if (polls!=3 || lines.size()!=17 || output.drain(8,[](const char *) {})!=0) return 3;
    std::string recovered;
    for (unsigned index=0;index<lines.size();++index)
    {
        if (lines[index].size()>=1900 || lines[index].find("part="+std::to_string(index)+" parts=17 ")==std::string::npos) return 4;
        const auto begin=lines[index].find("payload=");
        if (begin==std::string::npos) return 5;
        recovered+=lines[index].substr(begin+8);
    }
    if (recovered!=large) return 6;
    const std::string maximum(32767,'b');
    if (!output.append(maximum.c_str(),unsigned(maximum.size()),"[MGPU][M337] kind=job_part",7,10)) return 7;
    output.clear();
    const std::string oversized(32768,'c');
    if (output.append(oversized.c_str(),unsigned(oversized.size()),"bad",7,10) || !output.empty()) return 8;
    if (!output.append("short",5,"",7,11)) return 9;
    unsigned short_count=0;
    output.drain(8,[&](const char *line) { if (std::string(line)=="short") ++short_count; });
    if (short_count!=1 || !output.empty()) return 10;
    std::puts("PASS: 22KB records across three <=8-line polls, exact reconstruction, no duplicates, size limits and buffer reuse");
}
