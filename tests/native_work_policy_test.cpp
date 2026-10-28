#include "../src/native_work_policy.hpp"
#include <cstdio>
#include <limits>

int main()
{
    mgpu::native_work::selection_policy policy;
    if (policy.candidate(1) || policy.learn(0,0,2,true,false,3) || policy.learn(1,2,2,true,false,3) ||
        policy.learn(1,0,3,true,false,3) || policy.learn(1,0,2,false,false,3) ||
        policy.learn(1,0,2,true,true,3) || policy.learn(1,0,2,true,false,1.999) ||
        policy.learn(1,0,2,true,false,std::numeric_limits<double>::infinity())) return 1;
    for (unsigned origin=1;origin<=4;++origin)
        if (!policy.learn(origin,0,2,true,false,2)) return 2;
    if (policy.learn(5,0,2,true,false,3) || policy.learn(1,0,2,true,false,3) || policy.candidate(5)) return 3;
    for (unsigned small=0;small<564;++small) if (!policy.candidate(1+small%4)) return 4;
    if (policy.selected!=0) return 5;
    for (unsigned job=0;job<mgpu::native_work::general_job_limit;++job) if (!policy.claim(false)) return 6;
    if (policy.claim(false) || !policy.candidate(1)) return 7;
    for (unsigned job=0;job<16;++job) if (!policy.claim(true)) return 8;
    if (policy.claim(true) || policy.candidate(1) || mgpu::native_work::operation_limit*2+64>mgpu::native_work::timestamp_limit) return 9;
    if (mgpu::native_work::general_job_limit!=1024 || mgpu::native_work::job_limit!=1040) return 10;
    if (mgpu::native_work::operation_limit!=160 || mgpu::native_work::timestamp_limit!=384) return 11;
    mgpu::native_work::selection_policy tagged;
    if (!tagged.learn(1,0,3,true,false,2,true) || !tagged.candidate(1)) return 12;
    std::puts("PASS: opaque DIRECT >=2ms; 1024 general + 16 reserved MetaCommand jobs, exact exhaustion and 160 operation pairs/384 query slots");
}
