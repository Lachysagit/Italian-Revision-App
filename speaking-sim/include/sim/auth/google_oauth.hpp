#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

#include "sim/config.hpp"
#include "sim/store.hpp"

namespace sim {
namespace auth {

//Google sign-in, Authorization Code flow with PKCE.
//
//Not the Google Identity Services button: the token exchange is the same shape
//as the Gemini call this project already makes through httplib, so it adds no
//dependency, and one PUBLIC_ORIGIN drives both the redirect_uri and the dev /
//production split. The id_token that comes back is trusted by provenance - it
//arrives straight from oauth2.googleapis.com over TLS - so its payload is
//decoded and checked but its RS256 signature is not verified. Verification is
//only required when a token arrives by way of the browser, which is the case
//this flow avoids.

struct PendingLogin {
    std::string verifier;
    //the PKCE secret. Its SHA-256 went to Google with the redirect; the
    //original is sent with the code, proving this is the same client
    std::chrono::steady_clock::time_point created;
};

//state -> pending login, held in memory rather than in the Store: these live
//for one redirect round trip, and a row written per sign-in button click would
//outlive its usefulness by a long way. Single process, so a map is enough.
class LoginStates {
public:
    std::string create(std::string verifier);
    //returns the state parameter to send to Google

    bool take(const std::string& state, PendingLogin& out);
    //single use: a state that comes back twice is a replay, and the second
    //attempt finds nothing

private:
    void sweep();
    //drops anything past the TTL, run on insert so an abandoned sign-in cannot
    //accumulate. No timer thread for a map this small

    static constexpr std::chrono::minutes kTtl{10};

    std::mutex m_;
    std::unordered_map<std::string, PendingLogin> pending_;
};

std::string random_token(std::size_t bytes);
//base64url of RAND_bytes. Used for the PKCE verifier, the state parameter and
//the session cookie alike. Deliberately not the mt19937 that Session shuffles
//topics with: that one is seeded for variety, this one has to be unguessable

std::string pkce_challenge(const std::string& verifier);
//base64url(SHA256(verifier)), the S256 method

std::string authorize_url(const Config& config,
                          const std::string& state,
                          const std::string& challenge);

std::string redirect_uri(const Config& config);
//PUBLIC_ORIGIN + "/auth/callback". Google compares this as a literal string
//against the console's allowlist, and it must be identical in the authorize
//request and the token exchange

//the outcome of a callback. `error` is empty on success; when it is set, it is
//already phrased for a person to read on an error page.
struct SignInResult {
    bool ok = false;
    GoogleProfile profile;
    std::string error;
};

SignInResult exchange_code(const Config& config,
                           const std::string& code,
                           const std::string& verifier);
//posts to oauth2.googleapis.com/token and decodes the id_token it returns.
//Blocking, and called on a Crow socket thread: once per sign-in, with explicit
//timeouts, the same tradeoff /api/translate already makes

}  // namespace auth
}  // namespace sim
