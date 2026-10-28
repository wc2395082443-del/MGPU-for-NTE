#include "../src/d3d12_device_identity.hpp"
#include <cstring>

namespace
{
struct private_store
{
    GUID key{};
    std::uintptr_t value = 0;
    UINT bytes = 0;
    bool writable = true;
};

struct fake_device final : ID3D12Object
{
    private_store &store;
    ULONG refs = 1;
    explicit fake_device(private_store &source) : store(source) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **output) override
    {
        *output = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(ID3D12Object)) return E_NOINTERFACE;
        *output = static_cast<ID3D12Object *>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *size, void *output) override
    {
        if (guid != store.key || store.bytes == 0) return E_FAIL;
        if (*size < store.bytes) return E_INVALIDARG;
        *size = store.bytes;
        std::memcpy(output, &store.value, store.bytes);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void *input) override
    {
        if (!store.writable || size > sizeof store.value) return E_FAIL;
        store.key = guid;
        store.bytes = size;
        if (size != 0) std::memcpy(&store.value, input, size);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
};
}

int main()
{
    using mgpu::aligned::device_identity;
    private_store shared, other, unsupported;
    fake_device native(shared), proxy(shared), different(other), opaque(unsupported);
    const LUID adapter{17, 3}, another_adapter{18, 3};
    {
        device_identity binding;
        if (binding.matches(&native, adapter) || binding.bind(nullptr, adapter)) return 1;
        if (!binding.bind(&proxy, adapter) || !binding.marked()) return 2;
        if (!binding.matches(&proxy, adapter) || !binding.matches(&native, adapter)) return 3;
        if (!binding.bind(&native, adapter)) return 4;
        if (binding.matches(&different, adapter) || binding.bind(&different, adapter)) return 5;
        if (binding.matches(&native, another_adapter) || binding.matches(nullptr, adapter)) return 6;
        shared.bytes = 1;
        if (binding.matches(&native, adapter)) return 7;
        shared.bytes = sizeof shared.value;
        shared.value ^= 1;
        if (binding.matches(&native, adapter)) return 8;
        shared.value ^= 1;
        if (!binding.matches(&native, adapter)) return 9;
    }
    if (native.refs != 1 || proxy.refs != 1 || different.refs != 1) return 10;
    {
        device_identity binding;
        if (!binding.bind(&native, adapter) || !binding.matches(&proxy, adapter)) return 11;
    }
    unsupported.writable = false;
    {
        device_identity binding;
        if (!binding.bind(&opaque, adapter) || binding.marked()) return 12;
        if (!binding.matches(&opaque, adapter) || binding.matches(&different, adapter)) return 13;
    }
    return opaque.refs == 1 && native.refs == 1 && proxy.refs == 1 ? 0 : 14;
}
