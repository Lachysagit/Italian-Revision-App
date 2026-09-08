#include "sim/translate.hpp"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>

#include <httplib.h>
#include "crow/json.h"

namespace sim {

namespace {

constexpr const char* kHost = "https://translation.googleapis.com";
constexpr const char* kPath = "/language/translate/v2";

// Same reasoning as gemini_examiner.cpp: httplib's 300s default would park a
// socket thread for five minutes on one stalled call. A translate is a single
// short REST hop, so the read timeout is far tighter than the examiner's.
constexpr time_t kConnectTimeoutSeconds = 10;
constexpr time_t kReadTimeoutSeconds = 15;
constexpr time_t kWriteTimeoutSeconds = 10;

// One keep-alive Client per calling thread, for the same reason the examiner
// has one: httplib releases socket_mutex_ around send/recv, so a shared client
// is not safe. A separate client from the examiner's - different host, and
// sharing one would defeat both keep-alives.
httplib::Client& client() {
    thread_local httplib::Client cli = [] {
        httplib::Client c(kHost);
        c.set_keep_alive(true);
        c.set_connection_timeout(kConnectTimeoutSeconds);
        c.set_read_timeout(kReadTimeoutSeconds);
        c.set_write_timeout(kWriteTimeoutSeconds);
        return c;
    }();
    return cli;
}

// Cloud Translation HTML-escapes its output even under format=text, so "c'è"
// arrives as "c&#39;è" and lands in the box looking like a bug. One left to
// right pass rather than repeated find/replace: replacing &amp; in a separate
// sweep would go on to unescape whatever the first sweeps had just produced.
std::string unescape_entities(const std::string& text) {
    static const struct { const char* entity; char replacement; } kEntities[] = {
        {"&#39;", '\''}, {"&quot;", '"'}, {"&lt;", '<'},
        {"&gt;", '>'},   {"&amp;", '&'},
    };

    std::string out;
    out.reserve(text.size());

    for (std::size_t i = 0; i < text.size();) {
        bool matched = false;
        if (text[i] == '&') {
            for (const auto& entry : kEntities) {
                const std::size_t length = std::string(entry.entity).size();
                if (text.compare(i, length, entry.entity) == 0) {
                    out += entry.replacement;
                    i += length;
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            out += text[i];
            ++i;
        }
    }
    return out;
}

}  // namespace

std::string translate_text(const std::string& api_key,
                           const std::string& text,
                           const std::string& source_lang,
                           const std::string& target_lang) {
    crow::json::wvalue body;
    body["q"] = text;
    body["source"] = source_lang;
    body["target"] = target_lang;
    body["format"] = "text";
    //text, not html: the input is a sentence a student typed, not markup

    const std::string path = std::string(kPath) + "?key=" + api_key;
    //v2 takes the key as a query parameter. It never reaches a browser - this
    //call is made from the server, and the client only ever sees the result

    httplib::Result res =
        client().Post(path, body.dump(), "application/json");

    if (!res) {
        std::cerr << "translate failure: NETWORK - "
                  << httplib::to_string(res.error())
                  << " (no request reached the API)\n";
        throw std::runtime_error(
            "Translate request failed: " + httplib::to_string(res.error()));
    }
    if (res->status != 200) {
        std::cerr << "translate failure: HTTP " << res->status
                  << "\n  body: " << res->body << '\n';
        // Google's own error.message is the only thing that distinguishes a
        // disabled API from a billing problem from a bad key, and all three
        // arrive as a 403. Logged in full; the client gets a generic string.
        throw std::runtime_error(
            "Translate HTTP " + std::to_string(res->status));
    }

    crow::json::rvalue parsed = crow::json::load(res->body);
    if (!parsed) {
        throw std::runtime_error("Translate returned unreadable JSON");
    }

    // Every hop checked rather than chained: crow's rvalue::operator[] throws
    // "cannot find key" on a miss, which says nothing about what was missing.
    if (!parsed.has("data") || !parsed["data"].has("translations")) {
        throw std::runtime_error("Translate response carried no translations");
    }

    const crow::json::rvalue& translations = parsed["data"]["translations"];
    if (translations.t() != crow::json::type::List || translations.size() == 0) {
        throw std::runtime_error("Translate returned an empty translations list");
    }

    const crow::json::rvalue& first = translations[0];
    if (!first.has("translatedText") ||
        first["translatedText"].t() != crow::json::type::String) {
        throw std::runtime_error("Translate result carried no text");
    }

    return unescape_entities(std::string(first["translatedText"].s()));
    // .s() points into the buffer owned by `parsed`, so the copy has to happen
    // before that buffer dies with this frame
}

}  // namespace sim
