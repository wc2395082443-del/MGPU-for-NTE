#include "../src/game_work_policy.hpp"
#include <cstdio>

int main()
{
    using namespace mgpu::game_work;
    record_state normal;
    if (normal.submit() || normal.retire(true)) return 1;
    if (!normal.close() || normal.close()) return 2;
    if (!normal.submit() || normal.retire(false) || !normal.retire(true)) return 3;
    if (normal.submit() || !normal.replayed || normal.state != lifecycle::invalid) return 4;
    record_state cancelled;
    cancelled.reset();
    if (cancelled.state != lifecycle::cancelled || cancelled.close() || cancelled.submit()) return 5;
    record_state closed_reset;
    closed_reset.close();
    closed_reset.reset();
    if (closed_reset.state != lifecycle::cancelled) return 6;
    record_state submitted_reset;
    submitted_reset.close();
    submitted_reset.submit();
    submitted_reset.reset();
    if (submitted_reset.state != lifecycle::submitted || !submitted_reset.retire(true)) return 7;
    record_state replay;
    replay.close();
    replay.submit();
    if (replay.submit() || replay.retire(true) || !replay.replayed) return 8;
    std::puts("PASS: measurement lifecycle, reset safety, retirement and replay rejection");
    return 0;
}
