#include "Optimization/SrtWalker/SrtFlatSlotClasses.hpp"
#include "Optimization/SrtWalker/SrtInstructionPredicates.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace ShaderRecompiler::Detail {

namespace {

std::vector<std::uint32_t> ResourceFrontier(const IrResourcePlan& plan, std::uint32_t start) {
    std::vector<std::uint32_t> frontier;
    std::vector<std::uint8_t> seen(plan.controlFlow.size(), 0u);
    std::vector<std::uint32_t> pending{start};
    while (!pending.empty()) {
        const auto block = pending.back();
        pending.pop_back();
        if (block >= plan.controlFlow.size()) {
            frontier.push_back(block);
            continue;
        }
        if (seen[block] != 0u) continue;
        seen[block] = 1u;
        const auto& entry = plan.controlFlow[block];
        if (entry.condition != nullptr || !entry.sources.empty() || !entry.images.empty()) {
            frontier.push_back(block);
            continue;
        }
        pending.insert(pending.end(), entry.successors.begin(), entry.successors.end());
    }
    std::sort(frontier.begin(), frontier.end());
    frontier.erase(std::unique(frontier.begin(), frontier.end()), frontier.end());
    return frontier;
}

std::vector<std::uint8_t> ReachableBlocks(const IrResourcePlan& plan, std::uint32_t start) {
    std::vector<std::uint8_t> seen(plan.controlFlow.size(), 0u);
    std::vector<std::uint32_t> pending{start};
    while (!pending.empty()) {
        const auto block = pending.back();
        pending.pop_back();
        if (block >= plan.controlFlow.size() || seen[block] != 0u) continue;
        seen[block] = 1u;
        const auto& successors = plan.controlFlow[block].successors;
        pending.insert(pending.end(), successors.begin(), successors.end());
    }
    return seen;
}

bool SourceNeedsExactCondition(const IrResourcePlan& plan, std::uint32_t source) {
    if (plan.info.bdaWrites) return true;
    if (std::any_of(plan.info.images.begin(), plan.info.images.end(), [&](const ImageResource& image) { return image.source == source; })) return true;
    return std::any_of(plan.info.buffers.begin(), plan.info.buffers.end(), [&](const BufferResource& buffer) { return buffer.source == source && (buffer.written || buffer.atomic); });
}

bool BlockNeedsExactCondition(const IrResourcePlan& plan, const ResourceBlock& entry) {
    return !entry.images.empty() || std::any_of(entry.sources.begin(), entry.sources.end(), [&](std::uint32_t source) { return SourceNeedsExactCondition(plan, source); });
}

}

bool ConditionGuardsImagesOrWrites(const IrResourcePlan& plan, std::uint32_t block) {
    if (block >= plan.controlFlow.size()) return true;
    const auto& successors = plan.controlFlow[block].successors;
    if (successors.empty()) return false;
    std::vector<std::vector<std::uint8_t>> reached;
    reached.reserve(successors.size());
    for (const auto successor : successors) reached.push_back(ReachableBlocks(plan, successor));
    for (std::size_t index = 0; index < plan.controlFlow.size(); ++index) {
        const bool any = std::any_of(reached.begin(), reached.end(), [&](const std::vector<std::uint8_t>& seen) { return seen[index] != 0u; });
        const bool all = std::all_of(reached.begin(), reached.end(), [&](const std::vector<std::uint8_t>& seen) { return seen[index] != 0u; });
        if (any && !all && BlockNeedsExactCondition(plan, plan.controlFlow[index])) return true;
    }
    return false;
}

bool ConditionSelectsResources(const IrResourcePlan& plan, std::uint32_t block) {
    if (block >= plan.controlFlow.size()) return true;
    const auto& successors = plan.controlFlow[block].successors;
    if (successors.size() < 2u) return true;
    const auto frontier = ResourceFrontier(plan, successors.front());
    return std::any_of(successors.begin() + 1, successors.end(), [&](std::uint32_t successor) { return ResourceFrontier(plan, successor) != frontier; });
}

std::vector<std::uint8_t> ComputePureFlatSlots(const IrResourcePlan& plan) {
    std::vector<std::uint8_t> pure(plan.srtReads.size(), 1u);
    if (pure.empty()) return pure;
    const bool disqualified = !plan.srtPlanComplete || plan.requiresSpecializationMemory
        || std::any_of(plan.descriptorSources.begin(), plan.descriptorSources.end(), [](const DescriptorSource& source) { return source.tableColumn.has_value(); })
        || (plan.uniformFill.fill.kind != UniformFillKind::None && plan.uniformFill.fill.words > plan.uniformFill.values.size());
    if (disqualified) {
        std::fill(pure.begin(), pure.end(), 0u);
        return pure;
    }
    // The raw read node of each slot: a descriptor source dword may point at it directly (the
    // tracker resolved the handle argument before the plan builder patched the operands).
    std::unordered_map<const IrValue*, std::uint32_t> readSlots;
    for (std::uint32_t slot = 0; slot < plan.srtReads.size(); ++slot) {
        if (plan.srtReads[slot].value != nullptr) readSlots.emplace(plan.srtReads[slot].value->Resolve(), slot);
    }
    std::unordered_set<const IrValue*> visited;
    std::vector<IrValue*> pending;
    const auto mark = [&](IrValue* root) {
        if (root == nullptr) return;
        pending.push_back(root);
        while (!pending.empty()) {
            IrValue* value = pending.back()->Resolve();
            pending.pop_back();
            if (!visited.insert(value).second) continue;
            if (value->Opcode() == IrOpcode::ReadConst && value->ArgumentCount() >= 2 && value->Argument(1) != nullptr) {
                const IrValue* slot = value->Argument(1)->Resolve();
                if (slot->HasImmediate() && slot->Type() == IrType::U32 && slot->ImmediateU32() < pure.size()) {
                    const auto index = slot->ImmediateU32();
                    pure[index] = 0u;
                    pending.push_back(plan.srtReads[index].value);
                }
            }
            if (const auto found = readSlots.find(value); found != readSlots.end()) pure[found->second] = 0u;
            for (std::size_t i = 0; i < value->ArgumentCount(); ++i) {
                if (value->Argument(i) != nullptr) pending.push_back(value->Argument(i));
            }
        }
    };
    for (const auto& source : plan.descriptorSources) {
        for (std::uint32_t i = 0; i < source.dwordCount && i < source.dwords.size(); ++i) mark(source.dwords[i]);
    }
    for (const auto& block : plan.controlFlow) mark(block.condition);
    if (plan.uniformFill.fill.kind != UniformFillKind::None) {
        for (std::uint32_t i = 0; i < plan.uniformFill.fill.words; ++i) mark(plan.uniformFill.values[i]);
    }
    // The address cones: the arguments of every slot's raw read, never the read itself.
    for (const auto& read : plan.srtReads) {
        if (read.value == nullptr) continue;
        IrValue* raw = read.value->Resolve();
        for (std::size_t i = 0; i < raw->ArgumentCount(); ++i) mark(raw->Argument(i));
    }
    return pure;
}

std::vector<std::uint32_t> ComputeGuardedFlatSlots(const IrResourcePlan& plan) {
    std::unordered_map<const IrValue*, std::vector<const IrValue*>> users;
    std::unordered_set<const IrValue*> seen;
    std::vector<const IrValue*> pending;
    std::vector<const IrValue*> poisonable;
    for (const auto& read : plan.srtReads) {
        if (read.value != nullptr) pending.push_back(read.value->Resolve());
    }
    while (!pending.empty()) {
        const IrValue* value = pending.back();
        pending.pop_back();
        if (!seen.insert(value).second) continue;
        const IrValue* handle = value->ArgumentCount() != 0u && value->Argument(0) != nullptr ? value->Argument(0)->Resolve() : nullptr;
        if (IsRawRead(plan, *value) && handle != nullptr && LoadedFromMemory(*handle)) poisonable.push_back(value);
        const auto follow = [&](const IrValue* argument) {
            if (argument == nullptr) return;
            argument = argument->Resolve();
            users[argument].push_back(value);
            pending.push_back(argument);
        };
        for (std::size_t i = 0; i < value->ArgumentCount(); ++i) follow(value->Argument(i));
        if (value->Opcode() == IrOpcode::ReadConst && value->ArgumentCount() >= 2 && value->Argument(1) != nullptr) {
            const IrValue* slot = value->Argument(1)->Resolve();
            if (slot->HasImmediate() && slot->Type() == IrType::U32 && slot->ImmediateU32() < plan.srtReads.size()) follow(plan.srtReads[slot->ImmediateU32()].value);
        }
    }
    std::unordered_set<const IrValue*> reaching(poisonable.begin(), poisonable.end());
    while (!poisonable.empty()) {
        const IrValue* value = poisonable.back();
        poisonable.pop_back();
        const auto found = users.find(value);
        if (found == users.end()) continue;
        for (const IrValue* user : found->second) {
            if (reaching.insert(user).second) poisonable.push_back(user);
        }
    }
    std::vector<std::uint32_t> guarded;
    for (std::uint32_t slot = 0; slot < plan.srtReads.size(); ++slot) {
        if (plan.srtReads[slot].value != nullptr && reaching.contains(plan.srtReads[slot].value->Resolve())) guarded.push_back(slot);
    }
    return guarded;
}

}
