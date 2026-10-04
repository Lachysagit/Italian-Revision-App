#include "sim/http_util.hpp"

#include <cctype>
#include <cstddef>

namespace sim {

namespace {

std::string lowered(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

}  // namespace

crow::response json_response(const crow::json::wvalue& body, int status) {
    crow::response response(status, body.dump());
    response.set_header("Content-Type", "application/json");
    response.set_header("Cache-Control", "no-store");
    return response;
}

crow::response html_fragment(std::string body) {
    crow::response response(std::move(body));
    response.set_header("Content-Type", "text/html; charset=utf-8");
    response.set_header("Cache-Control", "no-store");
    return response;
}

crow::response json_error(int status, const std::string& message) {
    crow::json::wvalue json;
    json["error"] = message;
    return json_response(json, status);
}

std::string string_field(const crow::json::rvalue& body, const char* key) {
    if (!body || body.t() != crow::json::type::Object || !body.has(key) ||
        body[key].t() != crow::json::type::String) {
        return std::string();
    }
    return std::string(body[key].s());
}

std::optional<std::int64_t> int_field(const crow::json::rvalue& body,
                                      const char* key) {
    if (!body || body.t() != crow::json::type::Object || !body.has(key) ||
        body[key].t() != crow::json::type::Number) {
        return std::nullopt;
    }
    return body[key].i();
}

std::string cookie_value(const std::string& header, const std::string& name) {
    std::size_t at = 0;
    while (at < header.size()) {
        const std::size_t end = header.find(';', at);
        const std::string pair =
            header.substr(at, end == std::string::npos ? std::string::npos
                                                       : end - at);

        const std::size_t equals = pair.find('=');
        if (equals != std::string::npos) {
            const std::size_t key_start = pair.find_first_not_of(' ');
            const std::string key = pair.substr(key_start, equals - key_start);
            if (key == name) {
                std::string value = pair.substr(equals + 1);
                if (value.size() >= 2 && value.front() == '"' &&
                    value.back() == '"') {
                    value = value.substr(1, value.size() - 2);
                }
                return value;
            }
        }

        if (end == std::string::npos) break;
        at = end + 1;
    }
    return std::string();
    //the session token is URL-safe base64 without padding, so it never needs
    //unescaping. Quotes are stripped because RFC 6265 allows them
}

bool origin_allowed(const std::string& origin,
                    const std::string& host,
                    const std::string& public_origin) {
    if (origin.empty()) return false;
    if (lowered(origin) == lowered(public_origin)) return true;

    const std::size_t scheme_end = origin.find("://");
    if (scheme_end == std::string::npos || host.empty()) return false;
    return lowered(origin.substr(scheme_end + 3)) == lowered(host);
    //Origin is scheme://host[:port] with no path, and Host is host[:port], so
    //the part after the scheme compares directly. A page on another site can
    //set neither: the browser writes Origin and the address bar decides Host
}

bool same_origin_request(const crow::request& req,
                         const std::string& public_origin) {
    const std::string origin = req.get_header_value("Origin");
    if (origin.empty()) return true;
    return origin_allowed(origin, req.get_header_value("Host"), public_origin);
}

}  // namespace sim
