#include "../src/continuous_trace.hpp"
#include <array>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

struct sample
{
    std::uint64_t producer = 0, index = 0, inverse = 0, qpc = 0;
};

int main(int count, char **arguments)
{
    const bool long_test = count > 1 && std::string(arguments[1]) == "--120-seconds";
    const auto directory = std::filesystem::temp_directory_path();
    const auto path = directory / (L"mgpu-continuous-" + std::to_wstring(GetCurrentProcessId()) + L".bin");
    try
    {
        std::uint64_t accepted = 0;
        {
            mgpu::trace::continuous_stream<sample, 1024> stream;
            if (stream.push({}) || !stream.start(path, 73) || stream.start(path, 74))
                throw std::runtime_error("stream activation contract");
            const auto begin = GetTickCount64();
            std::atomic<std::uint64_t> total{0};
            std::array<std::thread, 4> producers;
            for (unsigned producer = 0; producer < producers.size(); ++producer)
                producers[producer] = std::thread([&, producer] {
                    std::uint64_t index = 0;
                    do
                    {
                        sample value{producer, index, ~index, mgpu::trace::ticks()};
                        if (stream.push(value)) ++total;
                        ++index;
                        if (long_test) Sleep(1);
                    } while (long_test ? GetTickCount64() - begin < 120100 : index < 80000);
                });
            for (auto &producer : producers) producer.join();
            if (!long_test)
                for (std::uint64_t index = 80000; index < 90000; ++index)
                {
                    const sample value{0, index, ~index, mgpu::trace::ticks()};
                    while (!stream.push(value)) Sleep(1);
                    ++total;
                }
            accepted = total.load();
            stream.finish();
            if (stream.push({})) throw std::runtime_error("write accepted after stop");
            const auto deadline = GetTickCount64() + 10000;
            while (!stream.done() && GetTickCount64() < deadline) Sleep(1);
            if (!stream.done() || stream.failures() || stream.written() != accepted || accepted <= 4096)
                throw std::runtime_error("continuous drain/count/old capacity contract");
        }
        std::ifstream file(path, std::ios::binary);
        mgpu::trace::stream_header header;
        file.read(reinterpret_cast<char *>(&header), sizeof header);
        if (!file || header.record_bytes != sizeof(sample) || header.records != accepted ||
            header.failures || header.end <= header.begin || !header.frequency)
            throw std::runtime_error("invalid sealed header");
        if (long_test && double(header.end - header.begin) / header.frequency < 120)
            throw std::runtime_error("full 120 seconds not observed");
        if (long_test && header.dropped) throw std::runtime_error("long trace lost records");
        std::array<std::uint64_t, 4> previous{};
        std::array<bool, 4> seen{};
        for (std::uint64_t index = 0; index < header.records; ++index)
        {
            sample value{};
            file.read(reinterpret_cast<char *>(&value), sizeof value);
            if (!file || value.producer >= previous.size() || value.inverse != ~value.index || !value.qpc ||
                (seen[value.producer] && value.index <= previous[value.producer]))
                throw std::runtime_error("torn, duplicated or out-of-order producer record");
            seen[value.producer] = true;
            previous[value.producer] = value.index;
        }
        if (file.peek() != std::char_traits<char>::eof()) throw std::runtime_error("unexpected trailing data");
        file.close();
        std::filesystem::remove(path);
        std::printf("PASS continuous trace: records=%llu dropped=%llu seconds=%.3f; bounded multi-producer storage and exact drain\n",
                    header.records, header.dropped, double(header.end - header.begin) / header.frequency);
        return 0;
    }
    catch (const std::exception &error)
    {
        std::printf("FAIL %s\n", error.what());
        return 1;
    }
}
