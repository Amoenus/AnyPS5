#include "BdaShader.hpp"
#include "BdaAbi.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace AgcDriver::Graphics;

class Pipeline {
public:
    Pipeline(const Context& context, std::span<const std::uint32_t> code, const std::array<Buffer*, 3>& buffers) : context(context) {
        try {
            std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
            for (std::uint32_t i = 0; i < bindings.size(); ++i) bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            setInfo.bindingCount = bindings.size();
            setInfo.pBindings = bindings.data();
            Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &setInfo, nullptr, &setLayout), "vkCreateDescriptorSetLayout");
            const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
            VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.maxSets = 1;
            poolInfo.poolSizeCount = 1;
            poolInfo.pPoolSizes = &size;
            Check(context.Function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(context.device, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool");
            VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, pool, 1, &setLayout};
            Check(context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(context.device, &allocation, &set), "vkAllocateDescriptorSets");
            for (std::uint32_t i = 0; i < buffers.size(); ++i) {
                const VkDescriptorBufferInfo buffer{buffers[i]->Handle(), 0, buffers[i]->Bytes().size()};
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = set;
                write.dstBinding = i;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo = &buffer;
                context.Function<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(context.device, 1, &write, 0, nullptr);
            }
            VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            layoutInfo.setLayoutCount = 1;
            layoutInfo.pSetLayouts = &setLayout;
            Check(context.Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(context.device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");
            VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            moduleInfo.codeSize = code.size_bytes();
            moduleInfo.pCode = code.data();
            Check(context.Function<PFN_vkCreateShaderModule>("vkCreateShaderModule")(context.device, &moduleInfo, nullptr, &module), "vkCreateShaderModule");
            VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
            pipelineInfo.layout = layout;
            Check(context.Function<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(context.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline), "vkCreateComputePipelines");
        } catch (...) { release(); throw; }
    }

    ~Pipeline() { release(); }

    void Run(std::uint32_t groups) {
        CommandBatch batch(context);
        const auto commands = batch.Handle();
        VkMemoryBarrier upload{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &upload, 0, nullptr, 0, nullptr);
        context.Function<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        context.Function<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        context.Function<PFN_vkCmdDispatch>("vkCmdDispatch")(commands, groups, 1, 1);
        VkMemoryBarrier download{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &download, 0, nullptr, 0, nullptr);
        batch.SubmitAndWait();
    }

private:
    void release() noexcept {
        if (pipeline) context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline")(context.device, pipeline, nullptr);
        if (module) context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
        if (layout) context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(context.device, layout, nullptr);
        if (pool) context.Function<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(context.device, pool, nullptr);
        if (setLayout) context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(context.device, setLayout, nullptr);
    }

    const Context& context;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

}

static std::uint32_t InitializeWrittenRanges(std::span<ShaderRecompiler::BdaAbi::Range> ranges) {
    namespace Abi = ShaderRecompiler::BdaAbi;
    std::uint64_t total = 0;
    for (auto& range : ranges) {
        const auto words = Abi::WrittenWords(range);
        Require(words <= std::numeric_limits<std::uint32_t>::max() - total, "test written bitmap is too large");
        range.writtenWord = words == 0u ? 0u : static_cast<std::uint32_t>(total);
        total += words;
    }
    return static_cast<std::uint32_t>(total);
}

static void RunBdaWrittenPageOverflowTests(const Context& context) {
    namespace Abi = ShaderRecompiler::BdaAbi;
    for (const std::uint32_t bucket : {0u, 4095u}) {
        std::vector<std::uint64_t> addresses;
        for (std::uint32_t page = 0x200000u; addresses.size() < 9u; ++page) {
            const auto hash = ((page + 1u) * 0x9e3779b1u) >> 20u;
            if (hash == bucket) addresses.push_back(static_cast<std::uint64_t>(page) << Abi::WrittenPageShift);
        }
        Buffer memory(context, addresses.size() * 16u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        std::memset(memory.Bytes().data(), 0xa5, memory.Bytes().size());
        std::vector<Abi::Range> ranges;
        for (std::size_t index = 0; index < addresses.size(); ++index) {
            ranges.push_back({addresses[index], addresses[index] + 8u, memory.DeviceAddress() + index * 16u, Abi::Read | Abi::Write, 0});
        }
        const auto writtenWords = InitializeWrittenRanges(ranges);
        Buffer table(context, sizeof(Abi::Header) + ranges.size() * sizeof(Abi::Range), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        Buffer fault(context, Abi::FaultBufferBytesForWords(writtenWords), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        Buffer output(context, 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        const Abi::Header header{Abi::Version, static_cast<std::uint32_t>(ranges.size()), sizeof(Abi::Range), writtenWords};
        std::memcpy(table.Bytes().data(), &header, sizeof(header));
        std::memcpy(table.Bytes().data() + sizeof(header), ranges.data(), ranges.size() * sizeof(Abi::Range));
        std::memset(fault.Bytes().data(), 0, fault.Bytes().size());
        for (std::size_t index = 0; index < addresses.size(); ++index) {
            const std::array<std::uint32_t, 2> values{static_cast<std::uint32_t>(index + 1u), 0x11223344u};
            Pipeline pipeline(context, MakeBdaDwordWriteTestShader(addresses[index], values.size(), values.data()), {&table, &fault, &output});
            pipeline.Run(1);
            if (index == 0u) pipeline.Run(1);
            std::array<std::uint32_t, 4> stored{};
            std::memcpy(stored.data(), memory.Bytes().data() + index * 16u, sizeof(stored));
            Require(stored[0] == values[0] && stored[1] == values[1] && stored[2] == 0xa5a5a5a5u && stored[3] == 0xa5a5a5a5u, "colliding BDA writes lost data or touched adjacent bytes");
            Abi::Fault report{};
            std::memcpy(&report, fault.Bytes().data(), sizeof(report));
            Require(report.state == Abi::FaultState::Empty, "colliding mapped BDA writes published an access fault");
            std::vector<std::uint32_t> words(fault.Bytes().size() / sizeof(std::uint32_t));
            std::memcpy(words.data(), fault.Bytes().data(), fault.Bytes().size());
            Require(words[Abi::WrittenErrorWord] == 0u, "colliding BDA writes failed bitmap accounting");
            for (std::size_t range = 0; range < ranges.size(); ++range) {
                Require(words[Abi::WrittenBitmapWord + ranges[range].writtenWord] == (range <= index ? 1u : 0u), "colliding BDA writes marked incorrect pages");
            }
        }
    }
    constexpr std::uint32_t pages = 4353;
    constexpr std::uint64_t guest = 0x407fa00000ULL;
    constexpr std::size_t pageBytes = std::size_t{1} << Abi::WrittenPageShift;
    Buffer memory(context, (pages + 1u) * pageBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    std::memset(memory.Bytes().data(), 0xa5, memory.Bytes().size());
    std::array<Abi::Range, 1> ranges{{{guest, guest + pages * pageBytes, memory.DeviceAddress(), Abi::Read | Abi::Write, 0}}};
    const auto writtenWords = InitializeWrittenRanges(ranges);
    Buffer table(context, sizeof(Abi::Header) + sizeof(ranges), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buffer fault(context, Abi::FaultBufferBytesForWords(writtenWords), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buffer output(context, 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const Abi::Header header{Abi::Version, ranges.size(), sizeof(Abi::Range), writtenWords};
    std::memcpy(table.Bytes().data(), &header, sizeof(header));
    std::memcpy(table.Bytes().data() + sizeof(header), ranges.data(), sizeof(ranges));
    std::memset(fault.Bytes().data(), 0, fault.Bytes().size());
    Pipeline pipeline(context, MakeBdaPageWriteTestShader(guest), {&table, &fault, &output});
    pipeline.Run(pages);
    Abi::Fault report{};
    std::memcpy(&report, fault.Bytes().data(), sizeof(report));
    Require(report.state == Abi::FaultState::Empty, "large BDA write published an access fault");
    std::vector<std::uint32_t> words(fault.Bytes().size() / sizeof(std::uint32_t));
    std::memcpy(words.data(), fault.Bytes().data(), fault.Bytes().size());
    Require(words[Abi::WrittenErrorWord] == 0u, "large BDA write failed bitmap accounting");
    for (std::uint32_t word = 0; word < writtenWords; ++word) {
        const auto remaining = pages - word * 32u;
        const auto expected = remaining >= 32u ? 0xffffffffu : (1u << remaining) - 1u;
        Require(words[Abi::WrittenBitmapWord + word] == expected, "large BDA write did not record all pages exactly");
    }
    for (std::size_t page = 0; page <= pages; ++page) {
        const auto bytes = memory.Bytes().subspan(page * pageBytes, pageBytes);
        if (page < pages) {
            std::array<std::uint32_t, 2> stored{};
            std::memcpy(stored.data(), bytes.data(), sizeof(stored));
            Require(stored == std::array<std::uint32_t, 2>{0x11223344u, 0x55667788u}, "large BDA write lost data");
        }
        const auto unchanged = bytes.subspan(page < pages ? 8u : 0u);
        Require(std::all_of(unchanged.begin(), unchanged.end(), [](auto byte) { return byte == std::byte{0xa5}; }), "large BDA write touched an unwritten byte");
    }
}

static void RunBdaWrittenBitmapRefusalTests(const Context& context) {
    namespace Abi = ShaderRecompiler::BdaAbi;
    constexpr std::uint64_t guest = 0x7fff12340000ULL;
    Buffer stored(context, 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    Buffer table(context, sizeof(Abi::Header) + sizeof(Abi::Range), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buffer output(context, 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const std::array<std::uint32_t, 2> values{0x11223344u, 0x55667788u};
    const auto reject = [&](Abi::Header header, Abi::Range range, std::size_t faultBytes) {
        Buffer fault(context, faultBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::memcpy(table.Bytes().data(), &header, sizeof(header));
        std::memcpy(table.Bytes().data() + sizeof(header), &range, sizeof(range));
        std::memset(fault.Bytes().data(), 0, fault.Bytes().size());
        std::memset(stored.Bytes().data(), 0xa5, stored.Bytes().size());
        Pipeline pipeline(context, MakeBdaDwordWriteTestShader(guest, 2, values.data()), {&table, &fault, &output});
        pipeline.Run(1);
        Abi::Fault report{};
        std::memcpy(&report, fault.Bytes().data(), sizeof(report));
        Require(report.state == Abi::FaultState::Ready && report.reason == Abi::FaultReason::InvalidTable && report.instruction == 0x1234u, "invalid BDA written bitmap did not publish its fault");
        Require(std::all_of(stored.Bytes().begin(), stored.Bytes().end(), [](auto byte) { return byte == std::byte{0xa5}; }), "invalid BDA written bitmap allowed a GPU store");
    };
    const Abi::Header header{Abi::Version, 1, sizeof(Abi::Range), 1};
    const Abi::Range range{guest, guest + 8, stored.DeviceAddress(), Abi::Read | Abi::Write, 0};
    auto invalidHeader = header;
    invalidHeader.version = 1;
    reject(invalidHeader, range, Abi::FaultBufferBytesForWords(1));
    invalidHeader = header;
    invalidHeader.writtenWords = 2;
    reject(invalidHeader, range, Abi::FaultBufferBytesForWords(1));
    auto invalidRange = range;
    invalidRange.writtenWord = 1;
    reject(header, invalidRange, Abi::FaultBufferBytesForWords(1));
    invalidRange.writtenWord = std::numeric_limits<std::uint32_t>::max();
    reject(header, invalidRange, Abi::FaultBufferBytesForWords(1));
    reject(header, range, sizeof(Abi::Fault));
}

static void RunBdaDwordWriteTests(const Context& context) {
    namespace Abi = ShaderRecompiler::BdaAbi;
    for (const std::uint64_t guest : {0x7fff12340f80ULL, 0x7fff12340ff1ULL}) {
        constexpr std::size_t firstBytes = 160;
        Buffer first(context, firstBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        Buffer second(context, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        Buffer readOnly(context, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        std::array<Abi::Range, 3> ranges{{
            {guest, guest + firstBytes, first.DeviceAddress(), Abi::Read | Abi::Write, 0},
            {guest + firstBytes, guest + firstBytes + 16, second.DeviceAddress(), Abi::Read | Abi::Write, 0},
            {guest + firstBytes + 16, guest + firstBytes + 32, readOnly.DeviceAddress(), Abi::Read, 0},
        }};
        const auto writtenWords = InitializeWrittenRanges(ranges);
        Buffer table(context, sizeof(Abi::Header) + sizeof(ranges), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        Buffer fault(context, Abi::FaultBufferBytesForWords(writtenWords), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        Buffer output(context, 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        const std::array<std::uint32_t, 4> values{0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u};
        const auto bytesAt = [&](std::uint64_t address) -> std::byte* {
            for (std::size_t index = 0; index < ranges.size(); ++index) {
                if (address < ranges[index].begin || address >= ranges[index].end) continue;
                auto& buffer = index == 0 ? first : index == 1 ? second : readOnly;
                return buffer.Bytes().data() + (address - ranges[index].begin);
            }
            return nullptr;
        };
        const auto noted = [&](std::uint64_t address) {
            for (const auto& range : ranges) {
                if (address < range.begin || address >= range.end || (range.permissions & Abi::Write) == 0u) continue;
                const auto relative = (address >> Abi::WrittenPageShift) - (range.begin >> Abi::WrittenPageShift);
                std::uint32_t word = 0;
                std::memcpy(&word, fault.Bytes().data() + (Abi::WrittenBitmapWord + range.writtenWord + relative / 32u) * sizeof(word), sizeof(word));
                return (word & (1u << (relative % 32u))) != 0u;
            }
            return false;
        };
        const auto write = [&](std::uint64_t address, std::uint32_t dwords, std::uint32_t storedBytes, Abi::FaultReason reason, std::uint32_t groups = 1) {
            const Abi::Header header{Abi::Version, static_cast<std::uint32_t>(ranges.size()), sizeof(Abi::Range), writtenWords};
            std::memcpy(table.Bytes().data(), &header, sizeof(header));
            std::memcpy(table.Bytes().data() + sizeof(header), ranges.data(), sizeof(ranges));
            std::memset(fault.Bytes().data(), 0, fault.Bytes().size());
            for (auto* buffer : {&first, &second, &readOnly}) std::memset(buffer->Bytes().data(), 0xa5, buffer->Bytes().size());
            Pipeline pipeline(context, MakeBdaDwordWriteTestShader(address, dwords, values.data()), {&table, &fault, &output});
            pipeline.Run(groups);
            Abi::Fault report{};
            std::memcpy(&report, fault.Bytes().data(), sizeof(report));
            for (std::uint32_t byte = 0; byte < dwords * 4u; ++byte) {
                const auto* stored = bytesAt(address + byte);
                const auto expected = byte < storedBytes ? static_cast<std::byte>(values[byte / 4u] >> ((byte % 4u) * 8u)) : std::byte{0xa5};
                Require(stored != nullptr && *stored == expected, "BDA dword write stored incorrect bytes at offset " + std::to_string(byte) + " of guest + " + std::to_string(address - guest));
            }
            Require(*bytesAt(address - 1) == std::byte{0xa5}, "BDA dword write touched the byte before it");
            if (address + dwords * 4u < guest + firstBytes + 32) Require(*bytesAt(address + dwords * 4u) == std::byte{0xa5}, "BDA dword write touched the byte after it");
            if (static_cast<std::uint32_t>(reason) == 0) {
                Require(report.state == Abi::FaultState::Empty, "a mapped BDA dword write published a fault");
            } else {
                Require(report.state == Abi::FaultState::Ready && report.reason == reason && report.instruction == 0x1234, "a BDA dword write did not publish its fault");
            }
            if (storedBytes != 0) Require(noted(address) && noted(address + storedBytes - 1u), "a BDA dword write did not note its written pages");
        };
        write(guest + 16, 4, 16, static_cast<Abi::FaultReason>(0));
        write(guest + 20, 4, 16, static_cast<Abi::FaultReason>(0));
        write(guest + 8, 2, 8, static_cast<Abi::FaultReason>(0));
        write(guest + 12, 2, 8, static_cast<Abi::FaultReason>(0));
        write(guest + 32, 3, 12, static_cast<Abi::FaultReason>(0));
        write(guest + 0x78, 4, 16, static_cast<Abi::FaultReason>(0));
        write(guest + 0x7c, 2, 8, static_cast<Abi::FaultReason>(0));
        write(guest + 41, 4, 16, static_cast<Abi::FaultReason>(0));
        write(guest + firstBytes - 8, 4, 16, static_cast<Abi::FaultReason>(0));
        write(guest + firstBytes + 12, 4, 4, Abi::FaultReason::Permission);
        write(guest + 0x7c, 2, 8, static_cast<Abi::FaultReason>(0), 64);
    }
}

void RunBdaExecutionTests(const Context& context) {
    namespace Abi = ShaderRecompiler::BdaAbi;
    Buffer first(context, 3, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    Buffer second(context, 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    first.Bytes()[0] = std::byte{0x11};
    first.Bytes()[1] = std::byte{0x22};
    first.Bytes()[2] = std::byte{0x33};
    second.Bytes()[0] = std::byte{0x44};
    second.Bytes()[1] = std::byte{0x55};
    constexpr std::uint64_t guest = 0x7fff12340001ULL;
    std::array<Abi::Range, 2> ranges{{{guest, guest + 3, first.DeviceAddress(), Abi::Read, 0}, {guest + 3, guest + 5, second.DeviceAddress(), Abi::Read, 0}}};
    Buffer table(context, sizeof(Abi::Header) + sizeof(ranges), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buffer fault(context, sizeof(Abi::Fault), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buffer output(context, 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const auto run = [&](std::uint64_t address, std::uint32_t bits, std::uint32_t expected, Abi::FaultReason reason, std::uint32_t count = 2, std::uint32_t groups = 1, std::int64_t offset = 0) {
        const Abi::Header header{Abi::Version, count, sizeof(Abi::Range), 0};
        std::memcpy(table.Bytes().data(), &header, sizeof(header));
        std::memcpy(table.Bytes().data() + sizeof(header), ranges.data(), sizeof(ranges));
        std::memset(fault.Bytes().data(), 0, fault.Bytes().size());
        const std::uint32_t sentinel = 0xdeadbeef;
        std::memcpy(output.Bytes().data(), &sentinel, sizeof(sentinel));
        Pipeline pipeline(context, MakeBdaTestShader(address, bits, offset), {&table, &fault, &output});
        pipeline.Run(groups);
        Abi::Fault report{};
        std::uint32_t result = 0;
        std::memcpy(&report, fault.Bytes().data(), sizeof(report));
        std::memcpy(&result, output.Bytes().data(), sizeof(result));
        if (static_cast<std::uint32_t>(reason) == 0) {
            Require(report.state == Abi::FaultState::Empty && result == expected, "BDA GPU read produced incorrect data or a fault");
        } else {
            Require(report.state == Abi::FaultState::Ready && report.reason == reason && report.instruction == 0x1234, "BDA GPU fault was not published correctly");
            Require(result == sentinel, "faulting BDA shader continued to output a substitute value");
        }
    };
    run(guest, 8, 0x11, static_cast<Abi::FaultReason>(0));
    run(guest + 1, 16, 0x3322, static_cast<Abi::FaultReason>(0));
    run(guest + 1, 32, 0x55443322, static_cast<Abi::FaultReason>(0));
    run(guest + 5, 8, 0, Abi::FaultReason::Unmapped, 2, 64);
    run(guest - 1, 8, 0, Abi::FaultReason::Unmapped);
    run(std::numeric_limits<std::uint64_t>::max() - 1, 32, 0, Abi::FaultReason::Overflow);
    run(guest, 8, 0, Abi::FaultReason::InvalidTable, 3);
    run(std::numeric_limits<std::uint64_t>::max() - 2, 8, 0, Abi::FaultReason::Overflow, 2, 1, 4);
    run(1, 8, 0, Abi::FaultReason::Overflow, 2, 1, -4);
    Buffer words(context, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const auto read = [&](std::uint64_t address, std::uint32_t dwords, bool coherent, bool stops, std::uint32_t value, bool faults) {
        const Abi::Header header{Abi::Version, 2, sizeof(Abi::Range), 0};
        std::memcpy(table.Bytes().data(), &header, sizeof(header));
        std::memcpy(table.Bytes().data() + sizeof(header), ranges.data(), sizeof(ranges));
        std::memset(fault.Bytes().data(), 0, fault.Bytes().size());
        const std::array<std::uint32_t, 4> sentinel{0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef};
        std::memcpy(words.Bytes().data(), sentinel.data(), sizeof(sentinel));
        Pipeline pipeline(context, MakeBdaDwordReadTestShader(address, dwords, coherent, stops), {&table, &fault, &words});
        pipeline.Run(1);
        Abi::Fault report{};
        std::array<std::uint32_t, 4> result{};
        std::memcpy(&report, fault.Bytes().data(), sizeof(report));
        std::memcpy(result.data(), words.Bytes().data(), sizeof(result));
        auto expected = sentinel;
        if (!faults || !stops) {
            expected[0] = value;
            for (std::uint32_t dword = 1; dword < dwords; ++dword) expected[dword] = 0;
        }
        if (faults) {
            Require(report.state == Abi::FaultState::Ready && report.reason == Abi::FaultReason::Unmapped && report.address == guest + 5 && report.bytes == 1 && report.instruction == 0x1234, "BDA dword read did not publish its first fault");
        } else {
            Require(report.state == Abi::FaultState::Empty, "mapped BDA dword read published a fault");
        }
        Require(result == expected, faults && stops ? "faulting BDA dword read continued to output a substitute value" : "BDA dword read produced incorrect data");
    };
    for (const bool stops : {true, false}) {
        for (const bool coherent : {false, true}) {
            read(guest + 1, 1, coherent, stops, 0x55443322, false);
            read(guest + 3, 1, coherent, stops, 0x5544, true);
            read(guest + 1, 4, coherent, stops, 0x55443322, true);
        }
    }
    Buffer wide(context, 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    for (std::uint32_t dword = 0; dword < 8u; ++dword) {
        const std::uint32_t value = 0xa0b0c000u + dword;
        std::memcpy(wide.Bytes().data() + dword * 4u, &value, sizeof(value));
    }
    constexpr std::uint64_t spanGuest = 0x7fff56780000ULL;
    const std::array<Abi::Range, 3> spanRanges{{ranges[0], ranges[1], {spanGuest, spanGuest + 32, wide.DeviceAddress(), Abi::Read, 0}}};
    Buffer spanTable(context, sizeof(Abi::Header) + sizeof(spanRanges), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const auto span = [&](std::uint64_t address, std::int32_t offset, std::uint32_t extracted, bool coherent, bool stops, std::array<std::uint32_t, 4> values, Abi::FaultReason reason = {}, std::uint64_t faultAddress = 0, std::uint32_t faultBytes = 0) {
        const Abi::Header header{Abi::Version, 3, sizeof(Abi::Range), 0};
        std::memcpy(spanTable.Bytes().data(), &header, sizeof(header));
        std::memcpy(spanTable.Bytes().data() + sizeof(header), spanRanges.data(), sizeof(spanRanges));
        std::memset(fault.Bytes().data(), 0, fault.Bytes().size());
        const std::array<std::uint32_t, 4> sentinel{0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef};
        std::memcpy(words.Bytes().data(), sentinel.data(), sizeof(sentinel));
        Pipeline pipeline(context, MakeBdaSpanReadTestShader(address, static_cast<std::uint32_t>(offset), extracted, coherent, stops), {&spanTable, &fault, &words});
        pipeline.Run(1);
        Abi::Fault report{};
        std::array<std::uint32_t, 4> result{};
        std::memcpy(&report, fault.Bytes().data(), sizeof(report));
        std::memcpy(result.data(), words.Bytes().data(), sizeof(result));
        const bool faults = static_cast<std::uint32_t>(reason) != 0u;
        if (faults) {
            Require(report.state == Abi::FaultState::Ready && report.reason == reason && report.address == faultAddress && report.bytes == faultBytes && report.instruction == 0x1234, "BDA span read did not publish its first fault");
        } else {
            Require(report.state == Abi::FaultState::Empty, "mapped BDA span read published a fault");
        }
        Require(result == (faults && stops ? sentinel : values), faults && stops ? "faulting BDA span read continued to output a substitute value" : "BDA span read produced incorrect data");
    };
    const auto at = [&](std::uint32_t dword) { return 0xa0b0c000u + dword; };
    for (const bool stops : {true, false}) {
        for (const bool coherent : {false, true}) {
            span(spanGuest + 16, -12, 0b1010u, coherent, stops, {0, at(2), 0, at(4)});
            span(spanGuest, 4, 0b0101u, coherent, stops, {at(1), 0, at(3), 0});
            span(spanGuest + 24, 0, 0b0011u, coherent, stops, {at(6), at(7), 0, 0});
            span(spanGuest + 20, 8, 0b0001u, coherent, stops, {at(7), 0, 0, 0});
            span(spanGuest + 28, 0, 0b0101u, coherent, stops, {at(7), 0, 0, 0}, Abi::FaultReason::Unmapped, spanGuest + 36, 1);
            span(guest, 1, 0b1001u, coherent, stops, {0x55443322, 0, 0, 0}, Abi::FaultReason::Unmapped, guest + 13, 1);
            span(4, -8, 0b0001u, coherent, stops, {0, 0, 0, 0}, Abi::FaultReason::Overflow, 4, 0);
        }
    }
    ranges[0].permissions = 0;
    run(guest, 8, 0, Abi::FaultReason::Permission);
    RunBdaDwordWriteTests(context);
    RunBdaWrittenBitmapRefusalTests(context);
    RunBdaWrittenPageOverflowTests(context);
}
