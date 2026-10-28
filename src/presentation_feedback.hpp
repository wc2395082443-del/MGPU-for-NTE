#pragma once

#include <dxgi1_3.h>
#include <array>
#include <cstdint>

namespace mgpu::presentation
{
struct stamp
{
    unsigned present_id = 0;
    std::uint64_t seal = 0, submitted_ns = 0;
    unsigned kind = 0, generated_index = 0;
};

class feedback
{
public:
    feedback() = default;
    feedback(const feedback &) = delete;
    feedback &operator=(const feedback &) = delete;
    ~feedback() { if (waitable_ != nullptr) CloseHandle(waitable_); }
    HRESULT initialize(IDXGISwapChain2 *chain, bool explicitly_owned)
    {
        if (chain == nullptr || !explicitly_owned) return E_INVALIDARG;
        DXGI_SWAP_CHAIN_DESC1 description{};
        HRESULT status = chain->GetDesc1(&description);
        if (FAILED(status)) return status;
        if ((description.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) == 0)
            return E_INVALIDARG;
        status = chain->SetMaximumFrameLatency(1);
        if (FAILED(status)) return status;
        const auto handle = chain->GetFrameLatencyWaitableObject();
        if (handle == nullptr) return E_FAIL;
        if (waitable_ != nullptr && waitable_ != handle) CloseHandle(waitable_);
        waitable_ = handle;
        acquired_ = false;
        return S_OK;
    }
    DWORD acquire(unsigned timeout_ms)
    {
        if (acquired_) return WAIT_OBJECT_0;
        if (waitable_ == nullptr) return WAIT_FAILED;
        const auto result = WaitForSingleObject(waitable_, timeout_ms);
        if (result == WAIT_OBJECT_0) acquired_ = true;
        else if (result == WAIT_TIMEOUT) ++capacity_timeouts_;
        return result;
    }
    void submitted(IDXGISwapChain *chain, std::uint64_t seal, unsigned kind, unsigned generated_index,
        std::uint64_t now_ns)
    {
        acquired_ = false;
        ++submissions_;
        unsigned id = 0;
        last_id_status_ = chain->GetLastPresentCount(&id);
        if (SUCCEEDED(last_id_status_))
            stamps_[id % stamps_.size()] = {id, seal, now_ns, kind, generated_index};
    }
    HRESULT statistics(IDXGISwapChain *chain, DXGI_FRAME_STATISTICS &result, stamp &matched) const
    {
        result = {};
        matched = {};
        const auto status = chain->GetFrameStatistics(&result);
        if (SUCCEEDED(status) && SUCCEEDED(last_id_status_))
        {
            const auto &entry = stamps_[result.PresentCount % stamps_.size()];
            if (entry.present_id == result.PresentCount) matched = entry;
        }
        return status;
    }
    bool active() const { return waitable_ != nullptr; }
    std::uint64_t submissions() const { return submissions_; }
    std::uint64_t capacity_timeouts() const { return capacity_timeouts_; }
private:
    HANDLE waitable_ = nullptr;
    bool acquired_ = false;
    std::uint64_t submissions_ = 0, capacity_timeouts_ = 0;
    HRESULT last_id_status_ = E_FAIL;
    std::array<stamp, 256> stamps_{};
};
}
