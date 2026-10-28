#include "../src/aligned_input.cpp"
#include <future>

namespace mgpu::diag
{
void info(const char *) {}
void warn(const char *) {}
}

namespace
{
unsigned forwarded = 0;
unsigned sequence = 0, execute_at = 0, local_at = 0, arrival_at = 0;
bool published_before_execute_returned = false;
struct fake_object { void **vtable; };
ID3D12Fence *local_fence = reinterpret_cast<ID3D12Fence *>(0x1000);
ID3D12Fence *arrival_fence = reinterpret_cast<ID3D12Fence *>(0x2000);
void STDMETHODCALLTYPE original_submit(ID3D12CommandQueue *, UINT count, ID3D12CommandList *const *)
{
    forwarded += 1 + count;
    execute_at = ++sequence;
    if (count != 0 && mgpu::aligned::submitted_color_frame() != 0)
        published_before_execute_returned = true;
}
HRESULT STDMETHODCALLTYPE command_data(ID3D12CommandList *, REFGUID, UINT *size, void *output)
{
    if (*size != sizeof(std::uint64_t)) return E_INVALIDARG;
    *static_cast<std::uint64_t *>(output) = 77;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE queue_signal(ID3D12CommandQueue *, ID3D12Fence *fence, UINT64)
{
    if (fence == local_fence) local_at = ++sequence;
    if (fence == arrival_fence) arrival_at = ++sequence;
    return S_OK;
}
}

int main()
{
    using namespace mgpu::aligned;
    data().hooks[0].original = &original_submit;
    void *list_methods[4]{};
    list_methods[3] = reinterpret_cast<void *>(&command_data);
    fake_object command{list_methods};
    void *queue_methods[15]{};
    queue_methods[14] = reinterpret_cast<void *>(&queue_signal);
    fake_object queue{queue_methods};
    auto *native_queue = reinterpret_cast<ID3D12CommandQueue *>(&queue);
    auto *native_command = reinterpret_cast<ID3D12CommandList *>(&command);
    data().queues[0] = {native_queue, local_fence, 0};
    data().commands[0] = {77, false, arrival_fence, 9};
    data().frames[0].state.serial = 1;
    data().frames[0].state.key = {123, 0};
    data().frames[0].state.commands = {77, 77, 77};
    data().frames[0].state.release_command = 77;
    execute<0>(native_queue, 1, &native_command);
    if (execute_at != 1 || local_at != 2 || arrival_at != 3 ||
        data().frames[0].state.copied[0].value != 1 ||
        data().frames[0].state.released.value != 1 ||
        data().commands[0].submitting || !data().commands[0].submitted ||
        published_before_execute_returned || submitted_color_frame() != ((1ull << 32) | 123u)) return 1;
    forwarded = 0;
    execute<0>(nullptr, 0, nullptr);
    data().failed = true;
    execute<0>(nullptr, 0, nullptr);
    stop();
    execute<0>(nullptr, 0, nullptr);
    std::unique_lock<std::recursive_mutex> tracked_submission(data().hooks[0].submission_mutex);
    auto untracked = std::async(std::launch::async, [] { execute<0>(nullptr, 0, nullptr); });
    const bool independent = untracked.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready;
    tracked_submission.unlock();
    untracked.get();
    return independent && forwarded == 4 ? 0 : 1;
}
