#include "../src/pipeline_timing.hpp"
#include <limits>

int main()
{
    using namespace mgpu::timing;
    const calibration clock{10000, 200000, 1000, 10000};
    auto sample = decode(9990, 10010, clock);
    if (sample.gpu_ms != 20 || sample.start_ms != 19990 || sample.end_ms != 20010) return 1;
    if (decode(0, 10, clock).gpu_ms != -1 || decode(11, 10, clock).gpu_ms != -1) return 2;
    if (decode(1, 2, {}).gpu_ms != -1) return 3;
    const calibration uncalibrated{0, 0, 1000, 10000};
    sample = decode(1, 2, uncalibrated);
    if (sample.gpu_ms != 1 || sample.start_ms != -1 || sample.end_ms != -1) return 4;
    if (interval(-1, 2) != -1 || interval(3, 2) != -1 || interval(2, 4) != 2) return 5;
    metric totals;
    totals.add(2);
    totals.add(6);
    totals.add(-1);
    totals.add(std::numeric_limits<double>::quiet_NaN());
    if (totals.count != 2 || totals.invalid != 2 || totals.mean() != 4 || totals.maximum != 6) return 6;
    const calibration large{UINT64_MAX - 100, UINT64_MAX - 100, 1000, 10000};
    if (decode(UINT64_MAX - 110, UINT64_MAX - 90, large).gpu_ms != 20) return 7;
    if (overlap({5, 10, 15}, {4, 12, 16}) != 3) return 8;
    if (overlap({5, 10, 15}, {4, 15, 19}) != 0) return 9;
    if (overlap({5, 10, 15}, {4, 20, 24}) != 0) return 10;
    if (overlap({}, {4, 20, 24}) != -1) return 11;
    if (overlap({1, 15, 10}, {4, 20, 24}) != -1) return 12;
    if (overlap({1, std::numeric_limits<double>::quiet_NaN(), 10}, {4, 20, 24}) != -1) return 13;
    return 0;
}
