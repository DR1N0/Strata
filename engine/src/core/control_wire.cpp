// Copyright (c) 2026 the Tectonic contributors (MIT).
#include "strata/core/control_wire.hpp"
#include <nlohmann/json.hpp>

namespace strata::core {
namespace {

using json = nlohmann::json;

bool send_json(BoundarySocket& sock, const json& j, std::string& err) {
    const std::string text = j.dump();
    if (text.size() > kControlMaxFrameBytes) {
        err = "control: outgoing message is " + std::to_string(text.size()) + " bytes, past the 8 MiB cap";
        return false;
    }
    const std::vector<uint8_t> bytes(text.begin(), text.end());
    return sock.send_frame(bytes, err);
}

// Returns false with `err` set on malformed JSON. A well-formed non-object (an array, a bare number) is
// reported the same way: the contract's messages are always objects, so anything else is as malformed as
// a parse failure. Split out from recv_json so a caller that already has the frame's bytes (entry_bridge's
// peek-then-dispatch - see control_parse_gen) does not have to read a second frame to get them.
bool parse_json_bytes(const std::vector<uint8_t>& bytes, json& out, std::string& err) {
    try {
        out = json::parse(bytes.begin(), bytes.end());
    } catch (const json::parse_error& e) {
        err = std::string("control: malformed JSON: ") + e.what();
        return false;
    }
    if (!out.is_object()) {
        err = "control: message is not a JSON object";
        return false;
    }
    return true;
}

bool recv_json(BoundarySocket& sock, json& out, std::string& err) {
    std::vector<uint8_t> bytes;
    if (!sock.recv_frame(bytes, err, kControlMaxFrameBytes)) return false;
    return parse_json_bytes(bytes, out, err);
}

const char* reject_reason_code(HelloRejectReason r) {
    switch (r) {
        case HelloRejectReason::kUnsupportedVersion: return "unsupported_version";
        case HelloRejectReason::kInstanceMismatch: return "instance_mismatch";
        case HelloRejectReason::kLayersMismatch: return "layers_mismatch";
    }
    return "unsupported_version";   // unreachable; silences -Wreturn-type on some compilers
}

}  // namespace

bool control_send_hello(BoundarySocket& sock, const HelloFields& f, std::string& err) {
    json j;
    j["type"] = "hello";
    j["v"] = kControlVersion;
    j["instance_id"] = f.instance_id;
    j["layers"] = {f.layer_begin, f.layer_end};
    j["batch"] = f.batch;
    j["model"] = {{"fingerprint", f.model_fingerprint}, {"layers", f.model_layers}};
    j["capabilities"] = {{"compute", f.cap_compute},
                         {"kv", f.cap_kv},
                         {"cancel", f.cap_cancel},
                         {"prefix_reuse", f.cap_prefix_reuse}};
    j["build"] = {{"name", f.build_name}, {"version", f.build_version}, {"platform", f.build_platform}};
    return send_json(sock, j, err);
}

bool control_recv_hello(BoundarySocket& sock, HelloFields& out, std::string& type_out, std::string& err) {
    json j;
    if (!recv_json(sock, j, err)) return false;
    type_out = j.value("type", std::string());
    if (type_out != "hello") {
        err = "control: expected hello, got " + (type_out.empty() ? std::string("a message with no type") : type_out);
        return false;
    }
    // Every field is read with a default rather than assumed present: a message that is missing one
    // of them is a protocol bug worth surfacing through whatever the field's absence does downstream
    // (e.g. an empty instance_id failing the handshake's own comparison), not a crash here.
    out.instance_id = j.value("instance_id", std::string());
    if (j.contains("layers") && j["layers"].is_array() && j["layers"].size() == 2) {
        out.layer_begin = j["layers"][0].get<int64_t>();
        out.layer_end = j["layers"][1].get<int64_t>();
    }
    out.batch = j.value("batch", 1);
    if (j.contains("model") && j["model"].is_object()) {
        out.model_fingerprint = j["model"].value("fingerprint", std::string());
        out.model_layers = j["model"].value("layers", int64_t(0));
    }
    if (j.contains("capabilities") && j["capabilities"].is_object()) {
        const auto& c = j["capabilities"];
        out.cap_compute = c.value("compute", false);
        out.cap_kv = c.value("kv", false);
        out.cap_cancel = c.value("cancel", false);
        out.cap_prefix_reuse = c.value("prefix_reuse", false);
    }
    if (j.contains("build") && j["build"].is_object()) {
        out.build_name = j["build"].value("name", std::string());
        out.build_version = j["build"].value("version", std::string());
        out.build_platform = j["build"].value("platform", std::string());
    }
    return true;
}

bool control_send_hello_ok(BoundarySocket& sock, const std::string& instance_id, std::string& err) {
    json j;
    j["type"] = "hello_ok";
    j["v"] = kControlVersion;
    j["instance_id"] = instance_id;
    return send_json(sock, j, err);
}

bool control_send_hello_reject(BoundarySocket& sock, HelloRejectReason reason, const std::string& message,
                               std::string& err) {
    json j;
    j["type"] = "hello_reject";
    j["reason"] = reject_reason_code(reason);
    j["message"] = message;
    return send_json(sock, j, err);
}

bool control_recv_hello_reply(BoundarySocket& sock, std::string& err) {
    json j;
    if (!recv_json(sock, j, err)) return false;
    const std::string type = j.value("type", std::string());
    if (type == "hello_ok") return true;
    if (type == "hello_reject") {
        err = "control: hello rejected (" + j.value("reason", std::string("?")) + "): " +
              j.value("message", std::string());
        return false;
    }
    err = "control: expected hello_ok or hello_reject, got " +
          (type.empty() ? std::string("a message with no type") : type);
    return false;
}

bool control_send_ready(BoundarySocket& sock, std::string& err) {
    json j;
    j["type"] = "ready";
    return send_json(sock, j, err);
}

bool control_recv_signal(BoundarySocket& sock, SignalMessage& out, std::string& err) {
    json j;
    if (!recv_json(sock, j, err)) return false;
    const std::string type = j.value("type", std::string());
    if (type == "cancel") {
        out.kind = SignalMessage::Kind::kCancel;
        out.request_id = j.value("request_id", std::string());
        return true;
    }
    if (type == "shutdown") {
        out.kind = SignalMessage::Kind::kShutdown;
        out.deadline_ms = j.value("deadline_ms", int64_t(0));
        return true;
    }
    err = "control: unexpected signal-connection message " +
          (type.empty() ? std::string("(no type)") : type);
    return false;
}

bool control_send_gen(BoundarySocket& sock, const GenFields& f, std::string& err) {
    json j;
    j["type"] = "gen";
    j["request_id"] = f.request_id;
    j["tokens"] = f.tokens;
    j["max_new"] = f.max_new;
    j["session_id"] = f.session_id;
    return send_json(sock, j, err);
}

namespace {
bool gen_from_json(const json& j, GenFields& out, std::string& type_out, std::string& err) {
    type_out = j.value("type", std::string());
    if (type_out != "gen") {
        err = "control: expected gen, got " + (type_out.empty() ? std::string("a message with no type") : type_out);
        return false;
    }
    out.request_id = j.value("request_id", std::string());
    out.tokens.clear();
    if (j.contains("tokens") && j["tokens"].is_array()) {
        for (const auto& t : j["tokens"]) out.tokens.push_back(t.get<int32_t>());
    }
    out.max_new = j.value("max_new", 0);
    out.session_id = j.value("session_id", uint64_t(0));
    return true;
}
}  // namespace

bool control_recv_gen(BoundarySocket& sock, GenFields& out, std::string& type_out, std::string& err) {
    json j;
    if (!recv_json(sock, j, err)) return false;
    return gen_from_json(j, out, type_out, err);
}

bool control_parse_gen(const std::vector<uint8_t>& bytes, GenFields& out, std::string& type_out, std::string& err) {
    json j;
    if (!parse_json_bytes(bytes, j, err)) return false;
    return gen_from_json(j, out, type_out, err);
}

bool control_send_delta(BoundarySocket& sock, const std::string& request_id, int32_t token, std::string& err) {
    json j;
    j["type"] = "delta";
    j["request_id"] = request_id;
    j["token"] = token;
    return send_json(sock, j, err);
}

bool control_send_done(BoundarySocket& sock, const std::string& request_id, const std::vector<int32_t>& tokens,
                       std::string& err) {
    json j;
    j["type"] = "done";
    j["request_id"] = request_id;
    j["tokens"] = tokens;
    return send_json(sock, j, err);
}

bool control_send_error(BoundarySocket& sock, const std::string& request_id, const std::string& code,
                        const std::string& message, std::string& err) {
    json j;
    j["type"] = "error";
    if (!request_id.empty()) j["request_id"] = request_id;
    j["code"] = code;
    j["message"] = message;
    return send_json(sock, j, err);
}

}  // namespace strata::core
