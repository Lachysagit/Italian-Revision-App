#include "sim/server.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "sim/http_util.hpp"

namespace sim {

// The class routes: what the teacher dashboard and the student's class picker
// read and write. Every handler follows one shape - check who is asking, check
// the body, make one Store call, answer in JSON - and every Store call sits
// inside guarded(), so a database error is a 500 with a fixed message rather
// than an exception loose on a Crow socket thread.

namespace {

constexpr std::size_t kMaxClassNameBytes = 80;
constexpr std::size_t kMaxInvitesPerRequest = 300;
constexpr std::size_t kMaxEmailBytes = 254;
constexpr int kAttemptListLimit = 200;

template <typename F>
crow::response guarded(const char* what, F&& handler) {
    try {
        return handler();
    } catch (const std::exception& e) {
        std::cerr << "class api: " << what << " failed: " << e.what() << '\n';
    } catch (...) {
        std::cerr << "class api: " << what << " failed with a non-std exception\n";
    }
    return json_error(500, "something went wrong, please try again");
    //the detail goes to the operator's log and a fixed string to the page, the
    //same split the examiner and translate paths use
}

std::string trimmed(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return std::string();
    return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

// Deliberately loose: one @, something before it, a dot somewhere after it, no
// spaces. The real check is Google's, when that person signs in; this only
// stops a stray word from a pasted roster becoming an invite nobody can claim.
bool looks_like_email(const std::string& email) {
    if (email.empty() || email.size() > kMaxEmailBytes) return false;
    const std::size_t at = email.find('@');
    if (at == 0 || at == std::string::npos || email.find('@', at + 1) != std::string::npos) {
        return false;
    }
    const std::size_t dot = email.find('.', at + 2);
    if (dot == std::string::npos || dot + 1 >= email.size()) return false;
    return std::none_of(email.begin(), email.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0 || ch == '<' || ch == '>' || ch == '"';
    });
}

// A roster pasted from a spreadsheet, an email client or a list typed by hand:
// separated by any mix of commas, semicolons, spaces and newlines.
std::vector<std::string> split_emails(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    const auto flush = [&] {
        if (!current.empty()) {
            out.push_back(current);
            current.clear();
        }
    };
    for (const char ch : text) {
        if (ch == ',' || ch == ';' || std::isspace(static_cast<unsigned char>(ch))) {
            flush();
        } else {
            current.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    flush();
    return out;
    //lowercased here so the dedupe below and the invite table agree; the
    //column is COLLATE NOCASE anyway, this only keeps the stored copy tidy
}

crow::json::wvalue class_json(const ClassInfo& klass,
                              const LanguageRegistry& languages) {
    crow::json::wvalue json;
    json["id"] = klass.id;
    json["name"] = klass.name;
    json["language"] = klass.language_id;
    const LanguagePack* pack = languages.find(klass.language_id);
    json["language_label"] = pack ? pack->display_name : klass.language_id;
    json["student_count"] = klass.student_count;
    json["archived"] = klass.archived;
    json["created_at"] = klass.created_at;

    const bool teaches = klass.caller_role == ClassRole::Teacher;
    json["role"] = klass.caller_role ? class_role_name(*klass.caller_role) : "";
    if (teaches) {
        json["join_code"] = klass.join_code;
        //a student never sees the code of a class they sit in: it is the
        //teacher's to hand out, and one forwarded by a student cannot be
        //traced back or withdrawn without rotating it for everybody
    }
    return json;
}

crow::json::wvalue attempt_json(const AttemptSummary& attempt) {
    crow::json::wvalue json;
    json["id"] = attempt.id;
    json["user_id"] = attempt.user_id;
    json["class_id"] = attempt.class_id;
    json["student_name"] = attempt.student_name;
    json["student_email"] = attempt.student_email;
    json["language"] = attempt.language_id;
    json["started_at"] = attempt.started_at;
    json["ended_at"] = attempt.ended_at;
    json["end_reason"] = attempt.end_reason;
    json["turn_count"] = attempt.turn_count;
    return json;
}

}  // namespace

void Server::register_class_routes() {
    CROW_ROUTE(app_, "/api/classes").methods("GET"_method, "POST"_method) //HTTP ROUTE ---
    ([this](const crow::request& req) {
        return serve_classes(req);
    });
    //GET lists the caller's classes, POST creates one. One path for both, the
    //usual REST shape, so the dashboard has one URL to remember

    CROW_ROUTE(app_, "/api/classes/<int>") //HTTP ROUTE ---
    ([this](const crow::request& req, std::int64_t class_id) {
        return serve_class(req, class_id);
    });

    CROW_ROUTE(app_, "/api/classes/<int>/join-code").methods("POST"_method) //HTTP ROUTE ---
    ([this](const crow::request& req, std::int64_t class_id) {
        return serve_join_code(req, class_id);
    });

    CROW_ROUTE(app_, "/api/classes/<int>/invites").methods("POST"_method) //HTTP ROUTE ---
    ([this](const crow::request& req, std::int64_t class_id) {
        return serve_invites(req, class_id);
    });

    CROW_ROUTE(app_, "/api/classes/<int>/invites/<int>").methods("DELETE"_method) //HTTP ROUTE ---
    ([this](const crow::request& req, std::int64_t class_id, std::int64_t invite_id) {
        return serve_revoke_invite(req, class_id, invite_id);
    });

    CROW_ROUTE(app_, "/api/classes/<int>/members/<int>").methods("DELETE"_method) //HTTP ROUTE ---
    ([this](const crow::request& req, std::int64_t class_id, std::int64_t user_id) {
        return serve_remove_member(req, class_id, user_id);
    });

    CROW_ROUTE(app_, "/api/classes/<int>/archive").methods("POST"_method) //HTTP ROUTE ---
    ([this](const crow::request& req, std::int64_t class_id) {
        return serve_archive(req, class_id);
    });

    CROW_ROUTE(app_, "/api/classes/<int>/attempts") //HTTP ROUTE ---
    ([this](const crow::request& req, std::int64_t class_id) {
        return serve_class_attempts(req, class_id);
    });

    CROW_ROUTE(app_, "/api/attempts/<int>") //HTTP ROUTE ---
    ([this](const crow::request& req, std::int64_t attempt_id) {
        return serve_attempt(req, attempt_id);
    });

    CROW_ROUTE(app_, "/api/join").methods("POST"_method) //HTTP ROUTE ---
    ([this](const crow::request& req) {
        return serve_join(req);
    });
    //not under /api/classes/: the student does not know the class id yet,
    //only the code, and a static segment beside <int> is a routing trap
}

std::optional<crow::response> Server::refuse_unless_signed_in(
    const crow::request& req, User& user) {
    if (req.method != crow::HTTPMethod::Get &&
        !same_origin_request(req, config_.public_origin)) {
        return json_error(403, "cross-site request refused");
        //SameSite=Lax already keeps the cookie off a cross-site POST in every
        //current browser; this is the second lock, for older ones and for the
        //day somebody relaxes the cookie
    }

    std::optional<User> found;
    try {
        found = user_for_request(req);
    } catch (const std::exception& e) {
        std::cerr << "class api: session lookup failed: " << e.what() << '\n';
        return json_error(500, "something went wrong, please try again");
    }
    if (!found) {
        return json_error(401, "not signed in");
    }
    user = std::move(*found);
    return std::nullopt;
}

std::optional<crow::response> Server::refuse_unless_teaches(
    const crow::request& req, std::int64_t class_id, User& user,
    ClassInfo& klass) {
    if (auto refusal = refuse_unless_signed_in(req, user)) {
        return refusal;
    }

    try {
        const auto found = store_->class_by_id(class_id);
        const auto role = found ? store_->class_role(class_id, user.id)
                                : std::nullopt;
        if (!found || role != ClassRole::Teacher) {
            return json_error(404, "no such class");
        }
        klass = *found;
        klass.caller_role = ClassRole::Teacher;
    } catch (const std::exception& e) {
        std::cerr << "class api: class lookup failed: " << e.what() << '\n';
        return json_error(500, "something went wrong, please try again");
    }
    return std::nullopt;
}

crow::response Server::serve_classes(const crow::request& req) {
    User user;
    if (auto refusal = refuse_unless_signed_in(req, user)) {
        return std::move(*refusal);
    }

    if (req.method == crow::HTTPMethod::Get) {
        return guarded("list classes", [&] {
            std::vector<crow::json::wvalue> list;
            for (const ClassInfo& klass : store_->classes_for_user(user.id)) {
                if (klass.caller_role != ClassRole::Teacher && klass.archived) {
                    continue;
                    //a student has nothing to do in an archived class; its
                    //teacher still sees it, to look back or to restore it
                }
                list.push_back(class_json(klass, languages_));
            }
            crow::json::wvalue json;
            json["classes"] = std::move(list);
            return json_response(json);
        });
    }

    if (!user.is_teacher) {
        return json_error(403, "only teachers can create classes");
    }

    const crow::json::rvalue body = crow::json::load(req.body);
    std::string name = trimmed(string_field(body, "name"));
    std::string language = string_field(body, "language");
    if (language.empty()) {
        language = string_field(body, "language_id");
    }
    //the sign-in gate's create-a-class step sends language_id, the dashboard
    //sends language; both name the same field, so either spelling is accepted

    const LanguagePack* pack = languages_.find(language);
    if (pack == nullptr) {
        return json_error(400, "pick a language");
        //checked against the registry, so a class can only be created for a
        //language this server can actually examine
    }

    if (name.empty()) {
        const std::string year = trimmed(string_field(body, "year_level"));
        if (!year.empty()) {
            name = pack->display_name + " Year " + year;
            //the gate asks for a cohort rather than a name, so the three
            //answers name the class. A teacher with two of them renames later
        }
    }
    if (name.empty() || name.size() > kMaxClassNameBytes) {
        return json_error(400, "give the class a name of up to 80 characters");
    }

    return guarded("create class", [&] {
        const ClassInfo created = store_->create_class(user.id, name, language);
        std::cerr << "classes: " << user.email << " created class " << created.id
                  << " (" << language << ")\n";
        crow::json::wvalue json;
        json["class"] = class_json(created, languages_);
        return json_response(json, 201);
    });
}

crow::response Server::serve_class(const crow::request& req,
                                   std::int64_t class_id) {
    User user;
    ClassInfo klass;
    if (auto refusal = refuse_unless_teaches(req, class_id, user, klass)) {
        return std::move(*refusal);
    }

    return guarded("class detail", [&] {
        std::vector<crow::json::wvalue> members;
        for (const ClassMember& member : store_->class_members(class_id)) {
            crow::json::wvalue json;
            json["user_id"] = member.user_id;
            json["name"] = member.display_name;
            json["email"] = member.email;
            json["role"] = class_role_name(member.role);
            json["year_level"] = member.year_level;
            json["subject_level"] = member.subject_level;
            json["added_at"] = member.added_at;
            json["attempt_count"] = member.attempt_count;
            json["last_attempt_at"] = member.last_attempt_at;
            members.push_back(std::move(json));
        }

        std::vector<crow::json::wvalue> invites;
        for (const ClassInvite& invite : store_->pending_invites(class_id)) {
            crow::json::wvalue json;
            json["id"] = invite.id;
            json["email"] = invite.email;
            json["created_at"] = invite.created_at;
            invites.push_back(std::move(json));
        }

        crow::json::wvalue json;
        json["class"] = class_json(klass, languages_);
        json["members"] = std::move(members);
        json["invites"] = std::move(invites);
        return json_response(json);
    });
}

crow::response Server::serve_join_code(const crow::request& req,
                                       std::int64_t class_id) {
    User user;
    ClassInfo klass;
    if (auto refusal = refuse_unless_teaches(req, class_id, user, klass)) {
        return std::move(*refusal);
    }

    const std::string action = string_field(crow::json::load(req.body), "action");
    if (action != "rotate" && action != "disable") {
        return json_error(400, "action must be rotate or disable");
    }
    if (klass.archived) {
        return json_error(409, "restore the class before changing its join code");
    }

    return guarded("join code", [&] {
        crow::json::wvalue json;
        if (action == "rotate") {
            json["join_code"] = store_->rotate_join_code(class_id);
        } else {
            store_->disable_join_code(class_id);
            json["join_code"] = "";
        }
        return json_response(json);
    });
}

crow::response Server::serve_invites(const crow::request& req,
                                     std::int64_t class_id) {
    User user;
    ClassInfo klass;
    if (auto refusal = refuse_unless_teaches(req, class_id, user, klass)) {
        return std::move(*refusal);
    }
    if (klass.archived) {
        return json_error(409, "restore the class before adding students");
    }

    const crow::json::rvalue body = crow::json::load(req.body);
    std::string raw = string_field(body, "emails");
    if (raw.empty() && body && body.t() == crow::json::type::Object &&
        body.has("emails") && body["emails"].t() == crow::json::type::List) {
        for (const auto& item : body["emails"]) {
            if (item.t() == crow::json::type::String) {
                raw += std::string(item.s());
                raw += '\n';
            }
        }
    }
    //a string straight from the textarea, or a list from a script. Both end up
    //as the same text and go through the same splitter

    std::vector<std::string> valid;
    std::vector<std::string> invalid;
    std::set<std::string> seen;
    for (std::string& email : split_emails(raw)) {
        if (!seen.insert(email).second) continue;
        (looks_like_email(email) ? valid : invalid).push_back(std::move(email));
    }

    if (valid.empty()) {
        return json_error(400, invalid.empty()
                                   ? "paste at least one email address"
                                   : "none of those look like email addresses");
    }
    if (valid.size() > kMaxInvitesPerRequest) {
        return json_error(400, "add at most 300 students at a time");
    }

    return guarded("invite", [&] {
        const InviteResult result = store_->invite_emails(class_id, valid, user.id);
        crow::json::wvalue json;
        json["invited"] = result.invited;
        json["added"] = result.added;
        json["existing"] = result.existing;
        json["invalid"] = invalid;
        //the rejects are handed back rather than dropped, so a typo in a pasted
        //roster is visible instead of quietly leaving one student out
        return json_response(json);
    });
}

crow::response Server::serve_revoke_invite(const crow::request& req,
                                           std::int64_t class_id,
                                           std::int64_t invite_id) {
    User user;
    ClassInfo klass;
    if (auto refusal = refuse_unless_teaches(req, class_id, user, klass)) {
        return std::move(*refusal);
    }

    return guarded("revoke invite", [&] {
        if (!store_->revoke_invite(class_id, invite_id)) {
            return json_error(404, "no such invite");
        }
        crow::json::wvalue json;
        json["ok"] = true;
        return json_response(json);
    });
}

crow::response Server::serve_remove_member(const crow::request& req,
                                           std::int64_t class_id,
                                           std::int64_t user_id) {
    User user;
    ClassInfo klass;
    if (auto refusal = refuse_unless_teaches(req, class_id, user, klass)) {
        return std::move(*refusal);
    }

    return guarded("remove member", [&] {
        const auto role = store_->class_role(class_id, user_id);
        if (!role) {
            return json_error(404, "that student is not in this class");
        }
        if (*role == ClassRole::Teacher) {
            return json_error(400, "teachers cannot be removed from here");
            //so the last teacher of a class cannot remove themselves by
            //accident and leave it with nobody able to manage it
        }
        store_->remove_member(class_id, user_id);
        crow::json::wvalue json;
        json["ok"] = true;
        return json_response(json);
    });
}

crow::response Server::serve_archive(const crow::request& req,
                                     std::int64_t class_id) {
    User user;
    ClassInfo klass;
    if (auto refusal = refuse_unless_teaches(req, class_id, user, klass)) {
        return std::move(*refusal);
    }

    const crow::json::rvalue body = crow::json::load(req.body);
    if (!body || body.t() != crow::json::type::Object || !body.has("archived") ||
        (body["archived"].t() != crow::json::type::True &&
         body["archived"].t() != crow::json::type::False)) {
        return json_error(400, "archived must be true or false");
    }
    const bool archived = body["archived"].t() == crow::json::type::True;

    return guarded("archive class", [&] {
        store_->set_archived(class_id, archived);
        crow::json::wvalue json;
        json["archived"] = archived;
        return json_response(json);
    });
    //archiving keeps everything - members, codes, history - and only hides the
    //class from its students and stops new joins. Nothing here deletes
}

crow::response Server::serve_class_attempts(const crow::request& req,
                                            std::int64_t class_id) {
    User user;
    ClassInfo klass;
    if (auto refusal = refuse_unless_teaches(req, class_id, user, klass)) {
        return std::move(*refusal);
    }

    return guarded("class attempts", [&] {
        std::vector<crow::json::wvalue> list;
        for (const AttemptSummary& attempt :
             store_->class_attempts(class_id, kAttemptListLimit)) {
            list.push_back(attempt_json(attempt));
        }
        crow::json::wvalue json;
        json["attempts"] = std::move(list);
        return json_response(json);
    });
}

crow::response Server::serve_attempt(const crow::request& req,
                                     std::int64_t attempt_id) {
    User user;
    if (auto refusal = refuse_unless_signed_in(req, user)) {
        return std::move(*refusal);
    }

    return guarded("attempt detail", [&] {
        const auto attempt = store_->attempt_by_id(attempt_id);
        bool allowed = attempt && attempt->user_id == user.id;
        if (attempt && !allowed && attempt->class_id > 0) {
            allowed =
                store_->class_role(attempt->class_id, user.id) == ClassRole::Teacher &&
                store_->class_role(attempt->class_id, attempt->user_id).has_value();
            //the class's teacher, and only while the student is still in the
            //class - the same rule class_attempts lists by
        }
        if (!allowed) {
            return json_error(404, "no such exam");
        }

        std::vector<crow::json::wvalue> turns;
        for (const AttemptTurn& turn : store_->attempt_turns(attempt_id)) {
            crow::json::wvalue json;
            json["index"] = turn.turn_index;
            json["role"] = turn.role;
            json["text"] = turn.text;
            json["topic"] = turn.topic;
            json["created_at"] = turn.created_at;
            turns.push_back(std::move(json));
        }

        crow::json::wvalue json;
        json["attempt"] = attempt_json(*attempt);
        json["turns"] = std::move(turns);
        return json_response(json);
    });
}

crow::response Server::serve_join(const crow::request& req) {
    User user;
    if (auto refusal = refuse_unless_signed_in(req, user)) {
        return std::move(*refusal);
    }

    const std::string code = string_field(crow::json::load(req.body), "code");
    if (code.empty()) {
        return json_error(400, "type the code your teacher gave you");
    }

    return guarded("join class", [&] {
        auto klass = store_->class_by_join_code(code);
        if (!klass) {
            return json_error(404,
                "that code did not match a class - check it with your teacher");
            //one message for wrong, switched off and archived alike, so the
            //answer says nothing about which codes once existed
        }

        const bool joined = store_->add_member(klass->id, user.id, ClassRole::Student);
        klass->caller_role = store_->class_role(klass->id, user.id);
        //read back rather than assumed: a teacher who uses their own code keeps
        //the teacher role add_member refused to overwrite
        if (joined) {
            ++klass->student_count;
            std::cerr << "classes: " << user.email << " joined class "
                      << klass->id << " with its code\n";
        }

        crow::json::wvalue json;
        json["class"] = class_json(*klass, languages_);
        json["joined"] = joined;
        return json_response(json, joined ? 201 : 200);
    });
}

}  // namespace sim
