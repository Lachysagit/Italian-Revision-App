#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "sim/exam_plan.hpp"
#include "sim/safety.hpp"
#include "sim/safety/adjudicator.hpp"

struct sqlite3;

namespace sim {

//accounts, classes and exam history. One sqlite3 handle guarded by one mutex:
//SQLITE_THREADSAFE=1 makes a single call safe, but not a prepare/bind/step
//sequence and not a transaction, so every public method below takes the lock
//for its whole body. Crow's socket threads and the worker pool both land here
//and the write volume is a few rows per turn, so contention is not a concern.
//
//Held as a member of Server, like LanguageRegistry. Unlike that registry it is
//mutable rather than const-after-startup, which is why Session must never hold
//a pointer to it: Session stays a pure state object and Server does the I/O.

enum class ClassRole {
    Teacher,
    Student,
};

struct User {
    std::int64_t id = 0;
    std::string email;
    std::string display_name;
    std::string picture_url;
    bool is_teacher = false;
    std::string year_level;
    //'9'..'12', for class grouping and reporting. Does not route keys
    std::string subject_level;
    //beginners|continuers|advanced|extension - this is what selects the key pool
    std::string preferred_language;
    //the exam language this student picked at sign-up. A preference, not a
    //restriction: the picker still offers the others
    bool onboarded = false;
    //false until a level has been chosen. No exam may start before it is true,
    //which is what stops a brand new account spending anything
};

struct GoogleProfile {
    std::string subject;   //Google's `sub`, stable forever - the join key
    std::string email;
    std::string display_name;
    std::string picture_url;
};

const char* class_role_name(ClassRole role);
std::optional<ClassRole> class_role_from_name(const std::string& name);
//the strings class_members.role and class_invites.role hold. Kept beside the
//enum so the two spellings cannot drift

struct ClassInfo {
    std::int64_t id = 0;
    std::string name;
    std::string language_id;
    std::string join_code;
    //empty when joining by code is switched off. Stored without the hyphen the
    //page shows, so a code typed either way matches
    std::int64_t owner_id = 0;
    std::int64_t created_at = 0;
    bool archived = false;
    int student_count = 0;
    std::optional<ClassRole> caller_role;
    //filled when the class was listed for one user, so a page can tell the
    //classes it teaches from the ones it sits in without a second query
};

struct ClassMember {
    std::int64_t user_id = 0;
    std::string email;
    std::string display_name;
    ClassRole role = ClassRole::Student;
    std::string year_level;
    std::string subject_level;
    std::int64_t added_at = 0;
    int attempt_count = 0;
    std::int64_t last_attempt_at = 0;
    //both counted within this class only: an exam a student sat privately, or
    //for another class, is not this teacher's to see
};

struct ClassInvite {
    std::int64_t id = 0;
    std::string email;
    std::int64_t created_at = 0;
};

struct InviteResult {
    int invited = 0;   //no account yet: joins the class the first time they sign in
    int added = 0;     //already had an account, so joined the class straight away
    int existing = 0;  //already a member or already invited, nothing to do
};

struct TurnFeature {
    int turn_index = 0;
    std::string kind;
    std::string value;
    std::string source;
};

struct RequiredQuestionStatus {
    std::int64_t question_id = 0;
    std::string text;
    std::string topic_group;
    std::string status;
    //pending while the exam runs, then asked or missed
    int turn_index = -1;
};

// One reply weighed against one set question. The verdict lives on
// RequiredQuestionStatus; this is the measurement it came from, kept so a
// verdict can be re-judged and so the thresholds can be checked against real
// exams rather than trusted.
struct QuestionEvidence {
    std::int64_t question_id = 0;
    int turn_index = 0;
    //the examiner turn that was weighed
    double overlap = 0.0;
    //share of the set question's words the reply carried, 0 to 1
    bool model_named = false;
    //whether the examiner labelled that reply with this question's id
};

struct CoverageRow {
    std::int64_t user_id = 0;
    std::string role;
    //whose turn it was: "student" counts tenses produced, "examiner" asked
    std::string kind;
    std::string value;
    int count = 0;
};

// ---- paid access and usage ------------------------------------------------

struct Licence {
    std::int64_t id = 0;
    std::string kind;
    //"user" or "class"
    std::int64_t target_id = 0;
    std::string target_label;
    //the account's email or the class's name, for the admin listing
    std::int64_t starts_at = 0;
    std::int64_t ends_at = 0;
    std::string note;
    bool revoked = false;
};

struct PaidAccess {
    bool active = false;
    std::string source;
    //"user" or "class": which kind of licence it came from
    std::int64_t until = 0;
};

struct AttemptSummary {
    std::int64_t id = 0;
    std::int64_t user_id = 0;
    std::int64_t class_id = 0;
    std::string student_name;
    std::string student_email;
    std::string language_id;
    std::int64_t started_at = 0;
    std::int64_t ended_at = 0;
    //zero while the exam is still running
    std::string end_reason;
    int turn_count = 0;
    std::string plan_name;
    //empty for an exam that followed no plan
    bool opinion_required = true;
    //whether this exam owed the student a question asking for an opinion. True
    //for a planless exam, and for a plan that left the box ticked
    int opinion_turn_index = -1;
    //the examiner turn that asked for one, or -1 if none did. Required and -1
    //on an ended attempt is the miss
    std::string opinion_source;
    //"model" or "openers" - which check saw it - and empty while none has
};

// One row of safety_events, read back for a teacher view or an incident
// export. Carries no sentence, for the reason the table's own comment gives.
struct SafetyEvent {
    std::int64_t id = 0;
    std::int64_t attempt_id = 0;
    int turn_index = 0;
    std::string stage;
    std::string action;
    std::string category;
    int severity = 0;
    std::string detector;
    std::string matches;
    //the normalised terms, comma separated and in the order they fired
    std::int64_t created_at = 0;

    std::string original_action;
    //non-empty only when the semantic pass moved the verdict. A cleared
    //escalation reads action="halt", original_action="escalate"
    std::string adjudication;
    //not_adjudicable | upheld | downgraded | unavailable
    std::string reason_code;

    std::string student_name;
    std::string student_email;
    //filled only by class_escalations, where the point of the row is which
    //student a teacher needs to go and find. Empty on the per-attempt read,
    //which is already scoped to one student
};

struct AttemptTurn {
    int turn_index = 0;
    std::string role;
    std::string text;
    std::string topic;
    std::int64_t created_at = 0;
};

class Store {
public:
    explicit Store(const std::string& path);
    //opens, applies the schema and reconciles attempts left open by a crash.
    //Throws on failure: a server that cannot record anything should not start
    ~Store();

    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    // ---- users and sign-in -----------------------------------------------

    User upsert_google_user(const GoogleProfile& profile, bool is_teacher);
    //matched on (provider, subject) first and on email second, so a teacher who
    //invited an address before that student ever signed in ends up on the same
    //row rather than creating a duplicate account beside the invite

    std::optional<User> user_by_id(std::int64_t user_id);

    void set_profile(std::int64_t user_id,
                     const std::string& year_level,
                     const std::string& subject_level,
                     const std::string& preferred_language);
    //onboarding. Stamps onboarded_at the first time, so a profile saved once is
    //the thing that lets an exam start; later edits leave the stamp alone

    bool is_in_any_class(std::int64_t user_id);
    //whether a profile edit is the user's own to make. A student in a class has
    //to ask a teacher; one in no class may change freely. Counts student seats
    //only: teaching a class says nothing about the teacher's own year level

    // ---- classes ---------------------------------------------------------

    ClassInfo create_class(std::int64_t owner_id,
                           const std::string& name,
                           const std::string& language_id);
    //the owner is added as the class's teacher in the same transaction, so a
    //class can never exist without somebody able to manage it

    std::vector<ClassInfo> classes_for_user(std::int64_t user_id);
    //every class the user sits in or teaches, archived ones included, with
    //caller_role set. Which of them a page shows is the page's decision

    std::optional<ClassInfo> class_by_id(std::int64_t class_id);

    std::optional<ClassInfo> class_by_join_code(const std::string& code);
    //the code as typed: case, spaces and hyphens are ignored. Archived classes
    //never match, so last year's code cannot pull a student into a dead class

    std::optional<ClassRole> class_role(std::int64_t class_id,
                                        std::int64_t user_id);
    //nullopt when the user is not in the class at all

    bool add_member(std::int64_t class_id, std::int64_t user_id, ClassRole role);
    //true when the user was newly added; an existing member keeps the role they
    //already had, so joining with a code cannot demote a teacher to a student

    bool remove_member(std::int64_t class_id, std::int64_t user_id);

    std::vector<ClassMember> class_members(std::int64_t class_id);

    std::string rotate_join_code(std::int64_t class_id);
    //returns the new code. The old one stops working at once, which is the
    //point: a code that leaked outside the class is replaced, not shared
    void disable_join_code(std::int64_t class_id);

    void set_archived(std::int64_t class_id, bool archived);

    InviteResult invite_emails(std::int64_t class_id,
                               const std::vector<std::string>& emails,
                               std::int64_t invited_by);
    //an address that already has an account is added to the class on the
    //spot; the rest wait in class_invites until claim_invites() meets them

    std::vector<ClassInvite> pending_invites(std::int64_t class_id);
    bool revoke_invite(std::int64_t class_id, std::int64_t invite_id);

    int claim_invites(const User& user);
    //called at every sign-in. Matched on email, which is exactly the fallback
    //upsert_google_user already uses to meet a roster entry. Returns how many
    //classes were joined

    // ---- exam history, read side ----------------------------------------

    std::vector<AttemptSummary> class_attempts(std::int64_t class_id, int limit);

    std::vector<AttemptSummary> user_attempts(std::int64_t user_id, int limit);
    //the student's own exams, class and private practice alike. Unlike the
    //class list this needs no membership check: they sat every one of them
    std::optional<AttemptSummary> attempt_by_id(std::int64_t attempt_id);
    std::vector<AttemptTurn> attempt_turns(std::int64_t attempt_id);

    // ---- exam plans ------------------------------------------------------

    std::vector<ExamPlan> class_plans(std::int64_t class_id, bool include_archived);
    std::optional<ExamPlan> plan_by_id(std::int64_t plan_id);

    ExamPlan save_plan(const ExamPlan& plan, std::int64_t user_id);
    //inserts when plan.id is 0, otherwise updates. Topics, questions and tense
    //targets are replaced wholesale in the same transaction: a plan is edited
    //as one form, so it is saved as one

    void set_default_plan(std::int64_t class_id, std::optional<std::int64_t> plan_id);
    void archive_plan(std::int64_t plan_id);
    //also stops it being the class default, so no exam starts on a plan the
    //teacher has put away

    void attach_plan(std::int64_t attempt_id, const ExamPlan& plan,
                     const std::string& plan_json);
    //the frozen copy, and one pending row per required question

    void record_turn_features(std::int64_t attempt_id, int turn_index,
                              const std::string& kind,
                              const std::vector<std::string>& values,
                              const std::string& source);

    void mark_required_question(std::int64_t attempt_id,
                                std::int64_t question_id,
                                const std::string& status,
                                int turn_index);

    void record_question_evidence(std::int64_t attempt_id, int turn_index,
                                 const std::vector<QuestionEvidence>& rows);
    //the scores this examiner turn earned against the still-open set questions.
    //turn_index comes from the caller rather than each row, so one turn's rows
    //cannot disagree about which turn they describe

    std::vector<QuestionEvidence> question_evidence(std::int64_t attempt_id);
    //every kept score for an attempt, by question then turn

    void mark_opinion_asked(std::int64_t attempt_id, int turn_index,
                            const std::string& source);
    //the exam's opinion question, asked on this examiner turn. First call wins,
    //so this is safe to call more than once

    void close_required_questions(std::int64_t attempt_id);
    //every question still pending when the exam ends becomes missed

    std::vector<TurnFeature> attempt_features(std::int64_t attempt_id);
    std::vector<RequiredQuestionStatus> attempt_required(std::int64_t attempt_id);

    std::vector<CoverageRow> class_coverage(std::int64_t class_id);
    //per current member, how many turns of their class exams carried each
    //tense (student and examiner turns counted apart) and each topic tag

    // ---- paid access and usage ------------------------------------------

    PaidAccess paid_access(std::int64_t user_id);
    //an unrevoked licence running now, on the account itself or on any
    //unarchived class the user is in. The latest end date wins

    std::int64_t grant_licence(const std::string& kind, std::int64_t target_id,
                               const std::string& until_date,
                               const std::string& note);
    //until_date is YYYY-MM-DD in the server's local time; the licence runs to
    //the end of that day. Starts now
    bool revoke_licence(std::int64_t licence_id);
    std::vector<Licence> licences();

    std::optional<User> user_by_email(const std::string& email);

    int usage_today(std::int64_t user_id, const std::string& feature);

    std::optional<int> reserve_usage(std::int64_t user_id,
                                     const std::string& feature, int limit);
    //spends one unit if the day's count is under limit, in one statement, so
    //two tabs racing for the last question cannot both get it. The new count,
    //or nullopt when the limit was already reached
    void release_usage(std::int64_t user_id, const std::string& feature);
    //gives one back: the turn it paid for failed on our side

    bool has_created_class(std::int64_t user_id);
    //whether this teacher owns a class already. What decides if the create-a-
    //class offer is shown: it is a first-time prompt, not a permanent one

    // ---- cookie sessions -------------------------------------------------

    std::string create_auth_session(std::int64_t user_id,
                                    const std::string& user_agent);
    //returns the raw token for the cookie. Only its SHA-256 is stored, so the
    //database file cannot be read for live logins

    std::optional<User> user_for_auth_token(const std::string& token);
    //also the websocket's check, so it must stay cheap: one indexed lookup

    void delete_auth_session(const std::string& token);

    // ---- exam attempts ---------------------------------------------------
    //the accessors for users, classes and cookie sessions arrive with the
    //phases that need them. These are the ones exam history needs.

    std::int64_t begin_attempt(std::optional<std::int64_t> user_id,
                               std::optional<std::int64_t> class_id,
                               const std::string& language_id,
                               const std::string& gemini_key_name);
    //user_id is nullopt for a browser that never signed in, which is only
    //possible while AUTH_REQUIRED is off. class_id is nullopt for private
    //practice, which no teacher can see. Returns 0 if the row could not be
    //written, which every caller below treats as "do not persist this attempt"
    //rather than as an error worth failing the exam over

    void record_turn(std::int64_t attempt_id,
                     int turn_index,
                     const std::string& role,
                     const std::string& text,
                     const std::string& topic,
                     long long stt_ms,
                     long long examiner_ms,
                     long long tts_ms);
    //also bumps the attempt's turn_count, so a dashboard does not have to count
    //rows to show how long an exam ran

    void note_examiner_call(std::int64_t attempt_id,
                            const std::string& gemini_key_name);
    //per call rather than per attempt: this is what least-used-today key
    //selection will read once pools exist

    // ---- safety ----------------------------------------------------------

    void record_safety_event(std::int64_t attempt_id,
                             int turn_index,
                             SafetyStage stage,
                             const SafetyVerdict& verdict,
                             const AdjudicationResult& adjudication);
    //an Allow verdict writes nothing UNLESS the semantic pass is what made it
    //Allow: a clean turn is not worth a row, but a trigger a model cleared
    //very much is. That single exception is what keeps "downgraded" auditable
    //rather than invisible

    std::vector<SafetyEvent> attempt_safety_events(std::int64_t attempt_id);
    //everything the chain caught in one exam, oldest first. The per-attempt
    //export A7 wants is built from this beside attempt_turns

    std::vector<SafetyEvent> class_escalations(std::int64_t class_id);
    //escalations only, newest first, across every attempt sat for one class.
    //This is the teacher's flag list - not a discipline report, which is why
    //masks and halts are deliberately left out of it

    void end_attempt(std::int64_t attempt_id, const std::string& reason);
    //WHERE ended_at IS NULL, so a disconnect arriving after a timer has already
    //closed the attempt cannot overwrite the more specific reason

private:
    void exec(const char* sql);
    void exec(const std::string& sql) { exec(sql.c_str()); }
    void migrate();
    void reconcile_crashed_attempts();
    std::string unused_join_code();
    //a fresh code no other class holds. Caller holds m_

    std::recursive_mutex m_;
    sqlite3* db_ = nullptr;
};

}  // namespace sim
