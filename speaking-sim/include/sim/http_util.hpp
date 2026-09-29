#pragma once

#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <string>

#include "crow.h"

namespace sim {

// The small pieces every JSON route and the websocket handshake share. They
// lived as file-local helpers in server.cpp until the class routes needed the
// same ones from a second file.

crow::response json_response(const crow::json::wvalue& body, int status = 200);
//Content-Type and no-store set once here: every one of these answers is about
//a signed-in person, and a shared cache holding one would hand it to the next

crow::response json_error(int status, const std::string& message);
//an error is JSON too, so the client can read .error the same way on every
//path instead of guessing whether a body is text or JSON by status code

template <typename F>
crow::response guarded(const char* what, F&& handler) {
    try {
        return handler();
    } catch (const std::exception& e) {
        std::cerr << "api: " << what << " failed: " << e.what() << '\n';
    } catch (...) {
        std::cerr << "api: " << what << " failed with a non-std exception\n";
    }
    return json_error(500, "something went wrong, please try again");
    //the detail goes to the operator's log and a fixed string to the page, the
    //same split the examiner and translate paths use. Every route that touches
    //the Store runs inside one, so a database error is a 500 rather than an
    //exception loose on a Crow socket thread
}

std::string string_field(const crow::json::rvalue& body, const char* key);
//empty when the key is missing or not a string. rvalue::operator[] throws on a
//missing key and .s() on a wrong type, so both are checked before reading

std::optional<std::int64_t> int_field(const crow::json::rvalue& body,
                                      const char* key);

std::string cookie_value(const std::string& header, const std::string& name);
//one cookie out of a raw Cookie header. The websocket handshake needs this
//because CookieParser only runs for ordinary routes, not for the upgrade

bool origin_allowed(const std::string& origin,
                    const std::string& host,
                    const std::string& public_origin);
//true when the Origin a browser sent is this server: either PUBLIC_ORIGIN
//exactly, or the same host the request was addressed to. The second form keeps
//127.0.0.1 and a LAN address working in development without a config change

bool same_origin_request(const crow::request& req,
                         const std::string& public_origin);
//for anything that changes state. A request carrying no Origin at all is let
//through: browsers always send one on a cross-site POST or websocket, so only a
//non-browser client - which holds no victim's cookie - arrives without it

}  // namespace sim
