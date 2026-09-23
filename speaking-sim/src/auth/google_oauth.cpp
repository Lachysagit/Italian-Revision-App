#include "sim/auth/google_oauth.hpp"

#include <openssl/rand.h>
#include <openssl/sha.h>

#include <cstdint>
#include <iostream>
#include <vector>

#include <httplib.h>

#include "crow/json.h"
#include "crow/utility.h"

namespace sim {
namespace auth {

namespace {

constexpr const char* kTokenHost = "https://oauth2.googleapis.com";
constexpr const char* kAuthorizeBase =
    "https://accounts.google.com/o/oauth2/v2/auth";

//cpp-httplib defaults to 300s, and this runs on a crow socket thread. The same
//reasoning as the examiner's client: one stalled call must not park a thread
//for five minutes.
constexpr time_t kConnectTimeoutSeconds = 10;
constexpr time_t kReadTimeoutSeconds = 20;
constexpr time_t kWriteTimeoutSeconds = 10;

std::string url_encode(const std::string& text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                                c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

//the payload of a JWT, decoded but not verified. See the note in the header:
//this token came straight from Google over TLS, so its provenance is the
//guarantee and a signature check would only repeat what the transport said.
bool decode_jwt_payload(const std::string& jwt, crow::json::rvalue& out) {
    const std::size_t first = jwt.find('.');
    if (first == std::string::npos) return false;
    const std::size_t second = jwt.find('.', first + 1);
    if (second == std::string::npos) return false;

    const std::string payload = jwt.substr(first + 1, second - first - 1);
    const std::string decoded = crow::utility::base64decode(payload, payload.size());
    //crow's decoder takes both the standard and the url-safe alphabet and
    //tolerates missing padding, which is exactly what a JWT segment is

    if (decoded.empty()) return false;
    out = crow::json::load(decoded);
    return static_cast<bool>(out);
}

bool has_string(const crow::json::rvalue& json, const char* key) {
    return json.has(key) && json[key].t() == crow::json::type::String;
    //presence and type, the discipline the rest of this codebase reads JSON
    //with. These fields come from Google, but a malformed reply must produce a
    //failed sign-in rather than an exception out of a socket thread
}

}  // namespace

std::string random_token(std::size_t bytes) {
    std::vector<unsigned char> buffer(bytes);
    if (RAND_bytes(buffer.data(), static_cast<int>(buffer.size())) != 1) {
        throw std::runtime_error("RAND_bytes failed: no secure randomness");
        //never silently fall back to a weaker source: this value is the only
        //thing standing between a guess and somebody else's session
    }
    std::string encoded =
        crow::utility::base64encode_urlsafe(buffer.data(), buffer.size());
    while (!encoded.empty() && encoded.back() == '=') {
        encoded.pop_back();
    }
    return encoded;
    //unpadded, like the PKCE challenge below. The '=' survives a URL only as
    //%3D, and these values travel as query parameters and cookie values alike
}

std::string pkce_challenge(const std::string& verifier) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(verifier.data()),
           verifier.size(), digest);

    std::string encoded =
        crow::utility::base64encode_urlsafe(digest, sizeof(digest));
    while (!encoded.empty() && encoded.back() == '=') {
        encoded.pop_back();
    }
    //S256 challenges are unpadded base64url. Google rejects the padding
    return encoded;
}

std::string redirect_uri(const Config& config) {
    return config.public_origin + "/auth/callback";
}

std::string authorize_url(const Config& config,
                          const std::string& state,
                          const std::string& challenge) {
    std::string url = kAuthorizeBase;
    url += "?client_id=" + url_encode(config.google_client_id);
    url += "&redirect_uri=" + url_encode(redirect_uri(config));
    url += "&response_type=code";
    url += "&scope=" + url_encode("openid email profile");
    url += "&state=" + url_encode(state);
    url += "&code_challenge=" + url_encode(challenge);
    url += "&code_challenge_method=S256";
    url += "&prompt=select_account";
    //select_account rather than none: a shared classroom machine must offer the
    //account picker instead of silently reusing whoever signed in last
    return url;
}

std::string LoginStates::create(std::string verifier) {
    std::string state = random_token(32);

    std::lock_guard<std::mutex> lock(m_);
    sweep();
    pending_.emplace(state, PendingLogin{std::move(verifier),
                                         std::chrono::steady_clock::now()});
    return state;
}

bool LoginStates::take(const std::string& state, PendingLogin& out) {
    std::lock_guard<std::mutex> lock(m_);

    auto it = pending_.find(state);
    if (it == pending_.end()) return false;

    const bool fresh =
        std::chrono::steady_clock::now() - it->second.created < kTtl;
    out = std::move(it->second);
    pending_.erase(it);
    //erased either way: an expired state is spent too, so a stale redirect
    //cannot be retried until it happens to race something

    return fresh;
}

void LoginStates::sweep() {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = pending_.begin(); it != pending_.end();) {
        it = (now - it->second.created >= kTtl) ? pending_.erase(it)
                                                : std::next(it);
    }
}

SignInResult exchange_code(const Config& config,
                           const std::string& code,
                           const std::string& verifier) {
    SignInResult result;

    httplib::Client client(kTokenHost);
    client.set_connection_timeout(kConnectTimeoutSeconds, 0);
    client.set_read_timeout(kReadTimeoutSeconds, 0);
    client.set_write_timeout(kWriteTimeoutSeconds, 0);

    const std::string body =
        "code=" + url_encode(code) +
        "&client_id=" + url_encode(config.google_client_id) +
        "&client_secret=" + url_encode(config.google_client_secret) +
        "&redirect_uri=" + url_encode(redirect_uri(config)) +
        "&grant_type=authorization_code"
        "&code_verifier=" + url_encode(verifier);

    const httplib::Result response =
        client.Post("/token", body, "application/x-www-form-urlencoded");

    if (!response) {
        result.error = "could not reach Google to complete sign-in";
        std::cerr << "oauth: token request failed to send\n";
        return result;
    }
    if (response->status != 200) {
        result.error = "Google refused the sign-in";
        std::cerr << "oauth: token endpoint returned " << response->status
                  << ": " << response->body << '\n';
        //the body names the real cause - redirect_uri_mismatch and
        //invalid_client are the two that cost the most time - and it must go to
        //the operator's log rather than to the person signing in
        return result;
    }

    const crow::json::rvalue token = crow::json::load(response->body);
    if (!token || !has_string(token, "id_token")) {
        result.error = "Google's reply could not be read";
        std::cerr << "oauth: token reply had no id_token\n";
        return result;
    }

    crow::json::rvalue claims;
    if (!decode_jwt_payload(token["id_token"].s(), claims)) {
        result.error = "Google's reply could not be read";
        std::cerr << "oauth: id_token payload did not decode\n";
        return result;
    }

    if (!has_string(claims, "aud") ||
        claims["aud"].s() != config.google_client_id) {
        result.error = "that sign-in was not issued for this app";
        std::cerr << "oauth: id_token aud did not match GOOGLE_CLIENT_ID\n";
        return result;
        //a token minted for a different client must not be accepted here even
        //though it is genuinely Google's
    }

    if (has_string(claims, "iss")) {
        const std::string issuer = claims["iss"].s();
        if (issuer != "accounts.google.com" &&
            issuer != "https://accounts.google.com") {
            result.error = "that sign-in did not come from Google";
            std::cerr << "oauth: unexpected id_token iss " << issuer << '\n';
            return result;
        }
    }

    if (!has_string(claims, "sub") || !has_string(claims, "email")) {
        result.error = "Google did not return an account to sign in as";
        std::cerr << "oauth: id_token was missing sub or email\n";
        return result;
    }

    const bool verified =
        claims.has("email_verified") &&
        (claims["email_verified"].t() == crow::json::type::True ||
         (claims["email_verified"].t() == crow::json::type::String &&
          claims["email_verified"].s() == "true"));
    if (!verified) {
        result.error = "that Google account has no verified email address";
        return result;
        //the email is the join key a teacher rosters against, so an unverified
        //one would let somebody claim an invite meant for another person
    }

    result.profile.subject = claims["sub"].s();
    result.profile.email = claims["email"].s();
    if (has_string(claims, "name")) {
        result.profile.display_name = claims["name"].s();
    }
    if (has_string(claims, "picture")) {
        result.profile.picture_url = claims["picture"].s();
    }

    if (result.profile.display_name.empty()) {
        const std::size_t at = result.profile.email.find('@');
        result.profile.display_name = result.profile.email.substr(0, at);
        //a profile with no name still has to give the examiner something to
        //call the student, and the local part is what a person would read out
    }

    result.ok = true;
    return result;
}

}  // namespace auth
}  // namespace sim
