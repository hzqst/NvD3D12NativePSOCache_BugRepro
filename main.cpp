#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

//------------------------------------------------------------------------------------------------
// Constants
//------------------------------------------------------------------------------------------------

//Two variants that differ only in the static sampler filter of their root signatures.
static constexpr UINT kVariantCount = 2;
static constexpr UINT kPoint = 0;
static constexpr UINT kLinear = 1;
static constexpr const wchar_t* kNames[kVariantCount] = {L"point", L"linear"};

static constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
static constexpr UINT kTextureWidth = 2;
static constexpr UINT kTargetWidth = 1;
static constexpr UINT kRowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
static constexpr UINT64 kReadbackStep = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
static constexpr UINT kFullscreenTriangleVertexCount = 3;
static constexpr UINT64 kFenceValue = 1;
static constexpr DWORD kGpuTimeoutMs = 30000;
static constexpr D3D12_RANGE kNoRange{0, 0};

using Pixel = std::array<unsigned char, 4>;
using Pixels = std::array<Pixel, kVariantCount>;

//Texture contents are red, green. Sampling at u=0.5 hits the texel boundary:
//point picks green, linear blends both texels.
static constexpr Pixel kTexels[kTextureWidth] = {{{255, 0, 0, 255}}, {{0, 255, 0, 255}}};
static constexpr Pixel kExpectedPoint = {0, 255, 0, 255};
//127 vs 128 is normal UNORM rounding. It cannot hide point/linear swapping.
static constexpr Pixel kExpectedLinearMin = {127, 127, 0, 255};
static constexpr Pixel kExpectedLinearMax = {128, 128, 0, 255};

static constexpr char kShaderSource[] = R"(
Texture2D<float4> tex : register(t0);
SamplerState samp : register(s0);
float4 VS(uint id : SV_VertexID) : SV_Position
{
    return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);
}
float4 PS() : SV_Target
{
    return tex.SampleLevel(samp, float2(0.5, 0.5), 0);
}
)";

//------------------------------------------------------------------------------------------------
// Command line
//------------------------------------------------------------------------------------------------

struct Options
{
    bool warm = false;
    bool reverseRoots = false;
    bool reversePsos = false;
    bool debug = false;
    bool listAdapters = false;
    bool help = false;
    UINT adapter = 0;
    std::filesystem::path cache = "pipeline-library.bin";
};

static void Check(HRESULT result, const char* operation)
{
    if (FAILED(result))
    {
        char message[256];
        std::snprintf(message, sizeof(message), "%s failed: HRESULT 0x%08X", operation, static_cast<unsigned>(result));
        throw std::runtime_error(message);
    }
}

static UINT ParseAdapterIndex(const char* text)
{
    UINT value = 0;
    const char* end = text + std::strlen(text);
    const auto parsed = std::from_chars(text, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end)
        throw std::runtime_error("Adapter index must be a nonnegative integer.");
    return value;
}

static Options ParseOptions(int argc, char** argv)
{
    if (argc < 2)
        throw std::runtime_error("Expected cold, warm, --list-adapters or --help.");

    Options options;
    const std::string mode = argv[1];
    if (mode == "warm")
        options.warm = true;
    else if (mode == "--list-adapters")
        options.listAdapters = true;
    else if (mode == "--help")
        options.help = true;
    else if (mode != "cold")
        throw std::runtime_error("Unknown mode: " + mode);

    for (int i = 2; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--reverse-roots")
            options.reverseRoots = true;
        else if (argument == "--reverse-psos")
            options.reversePsos = true;
        else if (argument == "--debug")
            options.debug = true;
        else if (argument == "--cache" || argument == "--adapter")
        {
            if (++i == argc)
                throw std::runtime_error("Missing value for " + argument);
            if (argument == "--cache")
                options.cache = std::filesystem::u8path(argv[i]);
            else
                options.adapter = ParseAdapterIndex(argv[i]);
        }
        else
            throw std::runtime_error("Unknown option: " + argument);
    }
    return options;
}

static void PrintUsage()
{
    std::puts("native_pso_repro --list-adapters\n"
              "native_pso_repro cold|warm [--adapter N] [--cache FILE] [--reverse-roots] [--reverse-psos] [--debug]\n"
              "Exit codes: 0 correct pixels; 1 incorrect pixels; 2 setup/API/validation error.\n"
              "cold creates and overwrites FILE; warm requires FILE and never creates replacement PSOs.");
}

static const char* OrderName(bool reverse)
{
    return reverse ? "linear,point" : "point,linear";
}

//Maps a creation/load step to a variant index, optionally in reverse order.
static UINT VariantAt(UINT step, bool reverse)
{
    return reverse ? kVariantCount - 1 - step : step;
}

//------------------------------------------------------------------------------------------------
// Adapters and debug layer
//------------------------------------------------------------------------------------------------

static void PrintAdapter(IDXGIAdapter1* adapter, UINT index)
{
    DXGI_ADAPTER_DESC1 desc{};
    Check(adapter->GetDesc1(&desc), "GetDesc1");
    std::printf("Adapter %u: %ls (vendor=0x%04X, device=0x%04X)\n", index, desc.Description, desc.VendorId, desc.DeviceId);

    LARGE_INTEGER version{};
    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version)))
    {
        std::printf("Driver: %u.%u.%u.%u\n", HIWORD(version.HighPart), LOWORD(version.HighPart),
            HIWORD(version.LowPart), LOWORD(version.LowPart));
    }
}

static void ListAdapters(IDXGIFactory1* factory)
{
    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT result = factory->EnumAdapters1(i, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND)
            break;
        Check(result, "EnumAdapters1");
        PrintAdapter(adapter.Get(), i);
    }
}

static void EnableDebugLayer()
{
    ComPtr<ID3D12Debug> debug;
    Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "D3D12GetDebugInterface (install Windows Graphics Tools)");
    debug->EnableDebugLayer();
}

static bool CheckDebugMessages(ID3D12Device* device)
{
    ComPtr<ID3D12InfoQueue> info;
    Check(device->QueryInterface(IID_PPV_ARGS(&info)), "Query debug info queue");

    bool valid = true;
    const UINT64 count = info->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i)
    {
        SIZE_T size = 0;
        Check(info->GetMessage(i, nullptr, &size), "Get debug message size");
        std::vector<unsigned char> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(info->GetMessage(i, message, &size), "Get debug message");
        std::printf("D3D12 message %u: %s\n", static_cast<unsigned>(message->ID), message->pDescription);
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            valid = false;
    }
    std::printf("Debug layer: %llu messages, %s\n", static_cast<unsigned long long>(count), valid ? "no errors" : "ERRORS");
    return valid;
}

//------------------------------------------------------------------------------------------------
// Pipeline library cache file
//------------------------------------------------------------------------------------------------

static std::vector<char> ReadCache(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot open cache; run cold first with the same adapter and driver.");
    std::vector<char> data(std::istreambuf_iterator<char>(file), {});
    if (file.bad() || data.empty())
        throw std::runtime_error("Cache is unreadable or empty.");
    return data;
}

static void SaveCache(ID3D12PipelineLibrary* library, const std::filesystem::path& path)
{
    std::vector<char> output(library->GetSerializedSize());
    Check(library->Serialize(output.data(), output.size()), "Serialize pipeline library");

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(output.data(), static_cast<std::streamsize>(output.size()));
    file.close();
    if (!file)
        throw std::runtime_error("Cannot write pipeline library.");
    std::printf("Saved %zu bytes of native pipeline-library data.\n", output.size());
}

//------------------------------------------------------------------------------------------------
// Shaders, root signatures and pipelines
//------------------------------------------------------------------------------------------------

static ComPtr<ID3DBlob> Compile(const char* entry, const char* target)
{
    ComPtr<ID3DBlob> code, errors;
    const HRESULT result = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, nullptr, nullptr, nullptr,
        entry, target, 0, 0, &code, &errors);
    if (FAILED(result) && errors)
        std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
    Check(result, "D3DCompile");
    return code;
}

//Identical bindings for every variant; only the static sampler filter differs.
static ComPtr<ID3D12RootSignature> CreateRoot(ID3D12Device* device, UINT variant)
{
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable = {1, &range};
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = variant == kPoint ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{1, &parameter, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> blob, errors;
    Check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors), "SerializeRootSignature");

    ComPtr<ID3D12RootSignature> root;
    Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
        "CreateRootSignature");
    return root;
}

//Shared graphics state; only the root signature differs between variants.
static D3D12_GRAPHICS_PIPELINE_STATE_DESC PipelineDesc(ID3D12RootSignature* root, ID3DBlob* vs, ID3DBlob* ps)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};

    auto& blend = desc.BlendState.RenderTarget[0];
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;

    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    desc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    desc.DepthStencilState.FrontFace = desc.DepthStencilState.BackFace = {
        D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};

    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = kFormat;
    desc.SampleDesc.Count = 1;
    return desc;
}

//Warm path: restores a named PSO from the library. Never falls back to CreateGraphicsPipelineState.
static ComPtr<ID3D12PipelineState> LoadPipeline(ID3D12PipelineLibrary* library, UINT variant,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc)
{
    ComPtr<ID3D12PipelineState> state;
    const HRESULT result = library->LoadGraphicsPipeline(kNames[variant], &desc, IID_PPV_ARGS(&state));
    std::printf("LoadGraphicsPipeline(%ls) = 0x%08X\n", kNames[variant], static_cast<unsigned>(result));
    Check(result, "LoadGraphicsPipeline");
    return state;
}

//Cold path: creates a fresh PSO and stores it in the library under its variant name.
static ComPtr<ID3D12PipelineState> CreateAndStorePipeline(ID3D12Device* device, ID3D12PipelineLibrary* library,
    UINT variant, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc)
{
    ComPtr<ID3D12PipelineState> state;
    Check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&state)), "CreateGraphicsPipelineState");
    Check(library->StorePipeline(kNames[variant], state.Get()), "StorePipeline");
    return state;
}

//------------------------------------------------------------------------------------------------
// Resources
//------------------------------------------------------------------------------------------------

static ComPtr<ID3D12Resource> CreateResource(ID3D12Device* device, D3D12_HEAP_TYPE type,
    const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;

    ComPtr<ID3D12Resource> resource;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource)),
        "CreateCommittedResource");
    return resource;
}

static D3D12_RESOURCE_DESC BufferDesc(UINT64 size)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

//Single-row, single-mip RGBA8 texture.
static D3D12_RESOURCE_DESC TextureDesc(UINT64 width, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = kFormat;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    return desc;
}

static D3D12_TEXTURE_COPY_LOCATION SubresourceLocation(ID3D12Resource* resource)
{
    D3D12_TEXTURE_COPY_LOCATION location{};
    location.pResource = resource;
    location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    return location;
}

static D3D12_TEXTURE_COPY_LOCATION FootprintLocation(ID3D12Resource* buffer, UINT64 offset, UINT width)
{
    D3D12_TEXTURE_COPY_LOCATION location{};
    location.pResource = buffer;
    location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    location.PlacedFootprint.Offset = offset;
    location.PlacedFootprint.Footprint = {kFormat, width, 1, 1, kRowPitch};
    return location;
}

static void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &barrier);
}

//------------------------------------------------------------------------------------------------
// Rendering
//------------------------------------------------------------------------------------------------

static void ExecuteAndWait(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list)
{
    Check(list->Close(), "Close command list");
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> fence;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    Check(queue->Signal(fence.Get(), kFenceValue), "Signal");

    const std::unique_ptr<void, decltype(&CloseHandle)> event(CreateEventW(nullptr, FALSE, FALSE, nullptr), CloseHandle);
    if (!event)
        throw std::runtime_error("CreateEvent failed.");
    Check(fence->SetEventOnCompletion(kFenceValue, event.get()), "SetEventOnCompletion");
    if (WaitForSingleObject(event.get(), kGpuTimeoutMs) != WAIT_OBJECT_0)
        throw std::runtime_error("GPU fence wait failed or timed out.");
    Check(device->GetDeviceRemovedReason(), "Device status after GPU completion");
}

static Pixels ReadPixels(ID3D12Resource* readback)
{
    void* mapped = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>((kVariantCount - 1) * kReadbackStep + sizeof(Pixel))};
    Check(readback->Map(0, &readRange, &mapped), "Map readback");

    Pixels result{};
    for (UINT i = 0; i < kVariantCount; ++i)
        std::memcpy(result[i].data(), static_cast<const unsigned char*>(mapped) + i * kReadbackStep, sizeof(Pixel));
    readback->Unmap(0, &kNoRange);
    return result;
}

//One queue and one command list; no window, swapchain, asynchronous compilation or resource reuse between processes.
static Pixels Render(ID3D12Device* device, const ComPtr<ID3D12RootSignature> (&roots)[kVariantCount],
    const ComPtr<ID3D12PipelineState> (&states)[kVariantCount])
{
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    ComPtr<ID3D12CommandQueue> queue;
    Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    ComPtr<ID3D12CommandAllocator> allocator;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
    ComPtr<ID3D12GraphicsCommandList> list;
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
        "CreateCommandList");

    //Upload the 2x1 source texture and make it readable by the pixel shader.
    auto texture = CreateResource(device, D3D12_HEAP_TYPE_DEFAULT, TextureDesc(kTextureWidth, D3D12_RESOURCE_FLAG_NONE),
        D3D12_RESOURCE_STATE_COPY_DEST);
    auto upload = CreateResource(device, D3D12_HEAP_TYPE_UPLOAD, BufferDesc(kRowPitch), D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    Check(upload->Map(0, &kNoRange, &mapped), "Map upload");
    std::memcpy(mapped, kTexels, sizeof(kTexels));
    upload->Unmap(0, nullptr);

    const auto textureCopy = SubresourceLocation(texture.Get());
    const auto uploadCopy = FootprintLocation(upload.Get(), 0, kTextureWidth);
    list->CopyTextureRegion(&textureCopy, 0, 0, 0, &uploadCopy, nullptr);
    Transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    //1x1 render target and one aligned readback slot per variant.
    auto target = CreateResource(device, D3D12_HEAP_TYPE_DEFAULT,
        TextureDesc(kTargetWidth, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET), D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto readback = CreateResource(device, D3D12_HEAP_TYPE_READBACK, BufferDesc(kReadbackStep * kVariantCount),
        D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = 1;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ComPtr<ID3D12DescriptorHeap> rtv;
    Check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtv)), "Create RTV heap");
    const auto rtvHandle = rtv->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, rtvHandle);

    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> srv;
    Check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srv)), "Create SRV heap");
    device->CreateShaderResourceView(texture.Get(), nullptr, srv->GetCPUDescriptorHandleForHeapStart());

    ID3D12DescriptorHeap* heaps[] = {srv.Get()};
    list->SetDescriptorHeaps(1, heaps);
    const D3D12_VIEWPORT viewport{0, 0, kTargetWidth, 1, 0, 1};
    const D3D12_RECT scissor{0, 0, kTargetWidth, 1};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    //Drawing order never changes: point first, linear second, each copied to its own aligned readback slot.
    const auto targetCopy = SubresourceLocation(target.Get());
    for (UINT i = 0; i < kVariantCount; ++i)
    {
        list->SetPipelineState(states[i].Get());
        list->SetGraphicsRootSignature(roots[i].Get());
        list->SetGraphicsRootDescriptorTable(0, srv->GetGPUDescriptorHandleForHeapStart());
        list->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
        list->DrawInstanced(kFullscreenTriangleVertexCount, 1, 0, 0);

        Transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        const auto readbackCopy = FootprintLocation(readback.Get(), i * kReadbackStep, kTargetWidth);
        list->CopyTextureRegion(&readbackCopy, 0, 0, 0, &targetCopy, nullptr);
        Transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    ExecuteAndWait(device, queue.Get(), list.Get());
    return ReadPixels(readback.Get());
}

//------------------------------------------------------------------------------------------------
// Validation
//------------------------------------------------------------------------------------------------

static bool IsWithin(const Pixel& pixel, const Pixel& low, const Pixel& high)
{
    for (size_t channel = 0; channel < pixel.size(); ++channel)
    {
        if (pixel[channel] < low[channel] || pixel[channel] > high[channel])
            return false;
    }
    return true;
}

static bool HasExpectedPixels(const Pixels& pixels)
{
    return pixels[kPoint] == kExpectedPoint && IsWithin(pixels[kLinear], kExpectedLinearMin, kExpectedLinearMax);
}

//------------------------------------------------------------------------------------------------
// Entry point
//------------------------------------------------------------------------------------------------

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try
    {
        const auto options = ParseOptions(argc, argv);
        if (options.help)
        {
            PrintUsage();
            return 0;
        }
        if (options.debug)
            EnableDebugLayer();

        ComPtr<IDXGIFactory4> factory;
        Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
        if (options.listAdapters)
        {
            ListAdapters(factory.Get());
            return 0;
        }

        ComPtr<IDXGIAdapter1> adapter;
        Check(factory->EnumAdapters1(options.adapter, &adapter), "EnumAdapters1");
        PrintAdapter(adapter.Get(), options.adapter);
        ComPtr<ID3D12Device1> device;
        Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");

        //CreatePipelineLibrary retains this pointer. Declare it before the library and never resize it on warm runs.
        const std::vector<char> input = options.warm ? ReadCache(options.cache) : std::vector<char>{};
        ComPtr<ID3D12PipelineLibrary> library;
        Check(device->CreatePipelineLibrary(input.empty() ? nullptr : input.data(), input.size(), IID_PPV_ARGS(&library)),
            "CreatePipelineLibrary");

        const auto vs = Compile("VS", "vs_5_0");
        const auto ps = Compile("PS", "ps_5_0");
        std::printf("Mode=%s, root order=%s, PSO order=%s\n", options.warm ? "warm" : "cold",
            OrderName(options.reverseRoots), OrderName(options.reversePsos));

        //Root creation order and PSO load order are controlled independently.
        ComPtr<ID3D12RootSignature> roots[kVariantCount];
        for (UINT step = 0; step < kVariantCount; ++step)
        {
            const UINT i = VariantAt(step, options.reverseRoots);
            roots[i] = CreateRoot(device.Get(), i);
        }

        ComPtr<ID3D12PipelineState> states[kVariantCount];
        for (UINT step = 0; step < kVariantCount; ++step)
        {
            const UINT i = VariantAt(step, options.reversePsos);
            const auto desc = PipelineDesc(roots[i].Get(), vs.Get(), ps.Get());
            states[i] = options.warm ? LoadPipeline(library.Get(), i, desc)
                                     : CreateAndStorePipeline(device.Get(), library.Get(), i, desc);
        }

        const auto pixels = Render(device.Get(), roots, states);
        for (UINT i = 0; i < kVariantCount; ++i)
            std::printf("%ls RGBA = (%u,%u,%u,%u)\n", kNames[i], pixels[i][0], pixels[i][1], pixels[i][2], pixels[i][3]);
        const bool correct = HasExpectedPixels(pixels);

        if (options.debug && !CheckDebugMessages(device.Get()))
            throw std::runtime_error("D3D12 validation errors; pixel results are not a valid reproduction.");
        //Only a verified cold run may produce the library consumed by warm runs.
        if (!options.warm && correct)
            SaveCache(library.Get(), options.cache);

        std::puts(correct ? "PASS: expected pixels"
                          : "WRONG_PIXELS: expected point=(0,255,0,255), linear=(127..128,127..128,0,255)");
        return correct ? 0 : 1;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "ERROR: %s\n", error.what());
        return 2;
    }
}
