// Copyright (c) 2026 the Tectonic contributors (MIT).
//
// entry_bridge.cpp - see entry_bridge.hpp for why this exists and what it deliberately does not do yet.
#include "strata/core/entry_bridge.hpp"
#include "strata/core/boundary_wire.hpp"

namespace strata::core {

bool EntryBridge::admit(const std::string& tectonic_request_id, int max_new, size_t max_in_flight,
                        uint64_t& wire_id) {
    std::lock_guard<std::mutex> lock(mu_);
    if (entries_.size() >= max_in_flight) return false;
    wire_id = next_wire_id_++;
    Entry& e = entries_[wire_id];
    e.tectonic_request_id = tectonic_request_id;
    e.max_new = max_new;
    e.tokens.clear();
    return true;
}

bool EntryBridge::resolve(uint64_t wire_id, std::string& tectonic_request_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(wire_id);
    if (it == entries_.end()) return false;
    tectonic_request_id = it->second.tectonic_request_id;
    return true;
}

bool EntryBridge::record_token(uint64_t wire_id, int32_t token, std::vector<int32_t>& tokens_so_far) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(wire_id);
    if (it == entries_.end()) {
        tokens_so_far.clear();
        return false;
    }
    it->second.tokens.push_back(token);
    tokens_so_far = it->second.tokens;
    return (int) it->second.tokens.size() < it->second.max_new;
}

bool EntryBridge::tokens_so_far(uint64_t wire_id, std::vector<int32_t>& out) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(wire_id);
    if (it == entries_.end()) {
        out.clear();
        return false;
    }
    out = it->second.tokens;
    return true;
}

void EntryBridge::release(uint64_t wire_id) {
    std::lock_guard<std::mutex> lock(mu_);
    entries_.erase(wire_id);
}

size_t EntryBridge::in_flight() const {
    std::lock_guard<std::mutex> lock(mu_);
    return entries_.size();
}

bool looks_like_boundary_frame(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 4) return false;
    const uint32_t magic = (uint32_t) bytes[0] | ((uint32_t) bytes[1] << 8) | ((uint32_t) bytes[2] << 16) |
                           ((uint32_t) bytes[3] << 24);
    return magic == kBoundaryMagic;
}

}  // namespace strata::core
