/* Copyright (c) 2026. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "cpu/o3/fp_wakeup.hh"

#include <algorithm>
#include <map>
#include <stdexcept>

namespace gem5
{
namespace o3
{

std::size_t
FpWakeupState::Hash::operator()(const FpWakeIdentity &id) const
{
    return std::hash<uint64_t>{}(id.seq) ^
        (std::hash<uint64_t>{}(id.attempt) << 1) ^
        (std::hash<uint32_t>{}(id.tid) << 2);
}

FpWakeupState::Token
FpWakeupState::prepare(FpWakeIdentity id, FpProducerKind kind,
                      unsigned channel, unsigned endpoint, uint64_t m2,
                      uint64_t m4, uint64_t data)
{
    if (tokens.count(id))
        throw std::logic_error("FP wakeup attempt must be unique");
    auto key = id;
    key.attempt = 0;
    auto old = latest.find(key);
    if (old != latest.end()) {
        if (auto previous = old->second.lock()) {
            if (id.attempt <= previous->producer.attempt)
                throw std::logic_error("FP wakeup attempt must increase");
            cancel(previous->producer);
        }
    }
    auto token = std::make_shared<WakeToken>();
    token->producer = id;
    token->kind = kind;
    token->outputChannel = channel;
    token->m4Endpoint = endpoint;
    token->m2Cycle = m2;
    token->m4Cycle = m4;
    token->dataCycle = data;
    tokens.emplace(id, token);
    latest[key] = token;
    peak = std::max(peak, tokens.size());
    return token;
}

FpWakeupState::Token
FpWakeupState::find(FpWakeIdentity id) const
{
    const auto it = tokens.find(id);
    return it == tokens.end() ? nullptr : it->second;
}

bool
FpWakeupState::current(const Token &token) const
{
    if (!token || token->canceled)
        return false;
    auto key = token->producer;
    key.attempt = 0;
    auto it = latest.find(key);
    return it != latest.end() && it->second.lock() == token;
}

bool
FpWakeupState::publish(FpWakeIdentity id)
{
    auto token = find(id);
    if (!current(token))
        return false;
    token->published = true;
    return true;
}

bool
FpWakeupState::usable(const Binding &binding)
{
    return binding && binding->token && !binding->canceled &&
        !binding->token->canceled && binding->ready;
}

bool
FpWakeupState::validateAdmission(FpWakeIdentity id) const
{
    auto token = find(id);
    if (!current(token) || token->faulted)
        return false;
    for (const auto &input : token->inputs) {
        if (auto binding = input.lock()) {
            if (!usable(binding) || !binding->token->confirmed ||
                binding->token->faulted)
                return false;
        } else {
            return false;
        }
    }
    return true;
}

bool
FpWakeupState::admit(FpWakeIdentity id)
{
    if (!validateAdmission(id))
        return false;
    find(id)->admitted = true;
    return true;
}

bool
FpWakeupState::confirm(FpWakeIdentity id, uint64_t cycle)
{
    auto token = find(id);
    if (!current(token) || !token->admitted || cycle < token->m2Cycle ||
        !validateAdmission(id))
        return false;
    token->confirmed = true;
    return true;
}

bool
FpWakeupState::complete(FpWakeIdentity id)
{
    auto token = find(id);
    if (!current(token) || !token->admitted || token->faulted)
        return false;
    token->dataReady = true;
    return true;
}

void
FpWakeupState::fault(FpWakeIdentity id)
{
    std::vector<Token> pending;
    if (auto token = find(id); current(token))
        pending.push_back(token);
    while (!pending.empty()) {
        auto token = pending.back();
        pending.pop_back();
        if (token->faulted || token->canceled)
            continue;
        token->faulted = true;
        for (const auto &user : token->users) {
            if (auto binding = user.lock()) {
                if (!binding->canceled) {
                    if (auto downstream = find(binding->consumer))
                        pending.push_back(downstream);
                }
            }
        }
    }
}

FpWakeupState::Binding
FpWakeupState::bind(FpWakeIdentity consumer, unsigned source, const Token &token,
                   FpWakePath path)
{
    auto binding = std::make_shared<SourceBinding>();
    binding->consumer = consumer;
    binding->source = source;
    binding->token = token;
    binding->path = path;
    binding->ready = current(token) && token->published &&
        (path == FpWakePath::M4 || token->m2Broadcast);
    if (token) {
        auto &users = token->users;
        users.erase(std::remove_if(users.begin(), users.end(),
            [](const auto &entry) {
                auto user = entry.lock();
                return !user || user->canceled;
            }), users.end());
        users.push_back(binding);
    }
    return binding;
}

void
FpWakeupState::addInput(FpWakeIdentity id, const Binding &binding)
{
    auto token = find(id);
    if (!current(token) || !binding)
        throw std::logic_error("Invalid FP wakeup input");
    for (const auto &input : token->inputs) {
        if (input.lock() == binding)
            return;
    }
    token->inputs.push_back(binding);
}

bool
FpWakeupState::select(const Binding &binding, uint64_t at_fu,
                      uint64_t late_read_delay)
{
    if (!usable(binding))
        return false;
    if (!binding->selected) {
        binding->selected = true;
        binding->deadline = at_fu +
            (binding->path == FpWakePath::M4 ? late_read_delay : 0);
    }
    return true;
}

FpDataStatus
FpWakeupState::checkData(const Binding &binding, uint64_t cycle)
{
    if (!usable(binding))
        return FpDataStatus::Canceled;
    if (binding->token->faulted)
        return FpDataStatus::FaultPending;
    if (binding->token->dataReady)
        return FpDataStatus::Ready;
    if (binding->selected && cycle >= binding->deadline)
        return FpDataStatus::MissedDeadline;
    return FpDataStatus::Pending;
}

bool
FpWakeupState::broadcastM2(FpWakeIdentity id, uint64_t cycle)
{
    if (!confirm(id, cycle))
        return false;
    auto token = find(id);
    token->m2Broadcast = true;
    for (auto it = token->users.begin(); it != token->users.end();) {
        auto binding = it->lock();
        if (!binding) {
            it = token->users.erase(it);
        } else {
            if (!binding->selected && !binding->canceled) {
                binding->path = FpWakePath::M2;
                binding->ready = true;
            }
            ++it;
        }
    }
    return true;
}

void
FpWakeupState::setRegister(uint64_t reg, const Token &token)
{
    if (current(token)) {
        registers[reg] = token;
        if (std::find(token->registers.begin(), token->registers.end(), reg) ==
            token->registers.end())
            token->registers.push_back(reg);
    }
}

FpWakeupState::Token
FpWakeupState::inheritM2(uint64_t reg) const
{
    const auto it = registers.find(reg);
    if (it == registers.end())
        return nullptr;
    auto token = it->second.lock();
    return current(token) && !token->faulted && token->confirmed && token->m2Broadcast ?
        token : nullptr;
}

void
FpWakeupState::clearRegister(uint64_t reg, const Token &expected)
{
    auto it = registers.find(reg);
    if (it != registers.end() && it->second.lock() == expected)
        registers.erase(it);
}

std::vector<FpWakeIdentity>
FpWakeupState::cancel(FpWakeIdentity id)
{
    std::vector<FpWakeIdentity> canceled;
    std::vector<Token> pending;
    if (auto token = find(id))
        pending.push_back(token);
    while (!pending.empty()) {
        auto token = pending.back();
        pending.pop_back();
        if (token->canceled)
            continue;
        token->canceled = true;
        token->confirmed = false;
        token->dataReady = false;
        canceled.push_back(token->producer);
        for (const auto &user : token->users) {
            if (auto binding = user.lock()) {
                binding->ready = false;
                binding->canceled = true;
                if (auto downstream = find(binding->consumer))
                    pending.push_back(downstream);
            }
        }
    }
    return canceled;
}

void
FpWakeupState::erase(FpWakeIdentity id)
{
    auto token = find(id);
    if (!token)
        return;
    auto key = id;
    key.attempt = 0;
    auto it = latest.find(key);
    if (it != latest.end() && it->second.lock() == token)
        latest.erase(it);
    tokens.erase(id);
    for (auto reg : token->registers)
        clearRegister(reg, token);
}

void
FpWakeupState::squash(uint32_t tid, uint64_t after_seq)
{
    std::vector<FpWakeIdentity> doomed;
    for (const auto &entry : tokens) {
        if (entry.first.tid == tid && entry.first.seq > after_seq)
            doomed.push_back(entry.first);
    }
    for (auto id : doomed) {
        cancel(id);
        erase(id);
    }
}

void
FpWakeupState::clear()
{
    for (auto &entry : tokens)
        cancel(entry.first);
    tokens.clear();
    latest.clear();
    registers.clear();
}

std::vector<FpWakeupState::Token>
FpWakeupState::arbitrateM4(const std::vector<Token> &candidates, uint64_t cycle)
{
    std::map<unsigned, Token> winners;
    for (const auto &token : candidates) {
        if (!token || token->canceled || !token->published ||
            token->m4Cycle != cycle)
            continue;
        auto &winner = winners[token->m4Endpoint];
        if (!winner ||
            (token->kind == FpProducerKind::Fma &&
             winner->kind != FpProducerKind::Fma) ||
            (token->kind == winner->kind &&
             token->producer.seq < winner->producer.seq))
            winner = token;
    }
    std::vector<Token> result;
    for (const auto &entry : winners)
        result.push_back(entry.second);
    return result;
}

} // namespace o3
} // namespace gem5
