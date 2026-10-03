#include "sim/session.hpp"

#include "sim/tenses.hpp"
#include "sim/topics.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace sim {

namespace {

bool has_visible_text(const std::string& text) {
    return std::any_of(text.begin(), text.end(), [](unsigned char ch) {
        return std::isspace(ch) == 0;
    });
    //.empty() tests length alone, so a whitespace-only transcript went to the
    //model as a blank turn. The unsigned char cast is required by isspace
}

// Two is a follow-up, three is a short thread, four is a rabbit hole.
int draw_topic_budget(std::mt19937& rng) {
    std::uniform_int_distribution<int> pick(2, 3);
    return pick(rng);
}

// Late enough that the student has warmed up, early enough that a short exam
// still reaches it.
int draw_opinion_target(std::mt19937& rng) {
    std::uniform_int_distribution<int> pick(4, 6);
    return pick(rng);
}

bool asks_opinion(const std::string& reply,
                  const std::vector<std::string>& openers) {
    std::string lowered;
    lowered.reserve(reply.size());
    for (const char ch : reply) {
        lowered.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch))));
    }

    for (const std::string& opener : openers) {
        if (lowered.find(opener) != std::string::npos) {
            return true;
        }
    }
    return false;
    //ASCII substrings only, so accented text folds past untouched. The openers
    //each language ships are chosen to be ASCII for exactly that reason: a
    //German "was haeltst du" written with its real umlaut would never match
    //here, and the only symptom would be an exam that quietly never asks for
    //an opinion
}

// The prompt files carry these where their topic list and tag list used to sit.
constexpr std::string_view kTopicMarker = "{{TOPICS}}";
constexpr std::string_view kTagMarker = "{{TOPIC_TAGS}}";

void replace_all(std::string& text, std::string_view marker, const std::string& value) {
    std::size_t at = text.find(marker);
    while (at != std::string::npos) {
        text.replace(at, marker.size(), value);
        at = text.find(marker, at + value.size());
    }
}

// Words for comparing a reply with a set question: lower case, split on
// anything that is not a letter. Accented letters are multi-byte UTF-8 and
// stay inside their word, the same way the tense rules split.
std::set<std::string> word_set(const std::string& text) {
    std::set<std::string> words;
    std::string word;
    for (const char raw : text) {
        const unsigned char ch = static_cast<unsigned char>(raw);
        if (std::isalpha(ch) != 0 || ch >= 0x80) {
            word.push_back(static_cast<char>(std::tolower(ch)));
        } else if (!word.empty()) {
            words.insert(word);
            word.clear();
        }
    }
    if (!word.empty()) words.insert(word);
    return words;
}

// How much of the set question the reply contains, from 0 to 1. The reply is
// allowed a reaction before the question, so this measures the question's
// words found in the reply, not the other way round.
double overlap(const std::string& wanted, const std::string& reply) {
    const std::set<std::string> want = word_set(wanted);
    if (want.empty()) return 0.0;
    const std::set<std::string> have = word_set(reply);
    std::size_t found = 0;
    for (const std::string& word : want) {
        found += have.count(word);
    }
    return static_cast<double>(found) / static_cast<double>(want.size());
}

constexpr auto kDefaultTurnPace = std::chrono::seconds(35);
constexpr auto kMinTurnPace = std::chrono::seconds(20);
constexpr auto kMaxTurnPace = std::chrono::seconds(90);
//one question and its answer, before any have been timed, and the bounds on
//the measured pace so one very quick or very slow turn cannot swing it

// The filled prompt, and the group the draw put first. Both prompts are filled
// from one draw per session, so the opening turn's samples and the menu the
// examiner reads name the same topic.
struct FilledPrompt {
    std::string text;
    std::string first_topic;
};

FilledPrompt fill_topics(std::string text, const TopicMenu& menu,
                         const std::string& tags) {
    replace_all(text, kTopicMarker, menu.text);
    replace_all(text, kTagMarker, tags);
    return FilledPrompt{std::move(text), menu.first};
    //a file with no marker comes back untouched, so an out-of-date prompt on
    //disk still runs an exam rather than stopping the server
}

}  // namespace

Session::Session()
    : rng_(std::random_device{}()) {
    topic_budget_ = draw_topic_budget(rng_);
    //up front, so an examiner that never tags a reply is still moved on
    opinion_target_ = draw_opinion_target(rng_);
}

void Session::set_language(const LanguagePack* pack) {
    if (pack == nullptr) {
        return;
        //keeps whatever was set before rather than leaving the session without
        //a language. Server passes the default on open, so there is always one
    }

    language_ = pack;
    question_bank_ = pack->question_bank;
    //shared_ptr to const, so every session reads the one copy loaded at startup
    rebuild_prompts();
}

void Session::rebuild_prompts() {
    if (language_ == nullptr) {
        return;
        //set_plan before any language: the prompts are built once one arrives
    }

    std::vector<std::string> allowed;
    std::string first;
    if (plan_) {
        for (const std::string& group : plan_->topics) {
            if (is_topic_group(group)) allowed.push_back(group);
        }
        for (const RequiredState& state : required_) {
            if (state.question.placement == "opening" &&
                is_topic_group(state.question.topic_group)) {
                first = state.question.topic_group;
                break;
            }
        }
    }

    const TopicMenu menu = topic_menu(rng_, allowed, first);
    std::string tags;
    for (const std::string& tag : tags_for_groups(allowed)) {
        if (!tags.empty()) tags += ", ";
        tags += tag;
    }

    first_prompt_ = fill_topics(language_->first_prompt, menu, tags).text;
    ongoing_prompt_ = fill_topics(language_->ongoing_prompt, menu, tags).text;
    opening_topic_ = menu.first;
    //one draw for both files, so the menu the opening turn reads and the menu
    //every later turn reads are the same list in the same order
    //the examiner only sees build_examiner_input(), so a prompt has to lead
    //the snapshot. The snapshot is rebuilt per call, so prompts cannot stack
    //the topic list is drawn here rather than per turn: the order holds for the
    //whole exam, so the examiner never sees the topics reshuffle under it
    //filled from the pack's copy into this session's own, so the draw is per
    //session while the file on disk is read once for the whole run
}

void Session::set_plan(ExamPlan plan) {
    required_.clear();
    for (const PlanQuestion& question : plan.questions) {
        required_.push_back(
            RequiredState{question, "q" + std::to_string(question.id)});
    }
    tenses_asked_.clear();
    for (const TenseTarget& target : plan.tenses) {
        tenses_asked_.emplace_back(target.tense, 0);
    }
    opinion_required_ = plan.require_opinion;
    //the order is gated on this rather than opinion_done_ being pre-set.
    //Marking the debt paid up front would also switch the detection off, and
    //an examiner that asks an opinion question anyway is worth recording: the
    //teacher then sees what the exam did, not only what it was told to do
    plan_ = std::move(plan);
    rebuild_prompts();
}

const std::optional<ExamPlan>& Session::plan() const {
    return plan_;
}

int Session::plan_duration_seconds() const {
    return plan_ ? plan_->duration_seconds : 0;
}

const LanguagePack& Session::language() const {
    return *language_;
    //never null in practice: Server sets the default on open, before the socket
    //can deliver a message that would reach this
}

void Session::set_gemini_key_name(std::string name) {
    gemini_key_name_ = std::move(name);
}

const std::string& Session::gemini_key_name() const {
    return gemini_key_name_;
}

bool Session::clock_started() const {
    std::lock_guard<std::mutex> lock(clock_mutex_);
    return deadline_.has_value();
}

void Session::start_clock(Clock::time_point now, Clock::duration length) {
    std::lock_guard<std::mutex> lock(clock_mutex_);
    if (!deadline_) {
        deadline_ = now + length;
        clock_started_at_ = now;
    }
    //first call wins: a second start must not buy the student more time
}

bool Session::time_up(Clock::time_point now) const {
    std::lock_guard<std::mutex> lock(clock_mutex_);
    return deadline_.has_value() && !paused_left_ && now >= *deadline_;
}

void Session::pause_clock(Clock::time_point now) {
    std::lock_guard<std::mutex> lock(clock_mutex_);
    if (!deadline_ || paused_left_) {
        return;
        //no clock yet, or already paused: pausing twice must not bank the
        //remainder a second time from a deadline that is no longer running
    }
    paused_left_ = *deadline_ > now ? *deadline_ - now : Clock::duration::zero();
    paused_at_ = now;
}

void Session::resume_clock(Clock::time_point now) {
    std::lock_guard<std::mutex> lock(clock_mutex_);
    if (!paused_left_) {
        return;
    }
    deadline_ = now + *paused_left_;
    paused_left_.reset();
    if (paused_at_) {
        paused_total_ += now - *paused_at_;
        paused_at_.reset();
    }
}

int Session::remaining_turns(Clock::time_point now) const {
    std::lock_guard<std::mutex> lock(clock_mutex_);
    if (!deadline_) return -1;

    const Clock::time_point measured_to = paused_at_ ? *paused_at_ : now;
    const Clock::duration left =
        paused_left_ ? *paused_left_
                     : (*deadline_ > now ? *deadline_ - now : Clock::duration::zero());

    Clock::duration pace = kDefaultTurnPace;
    if (questions_asked_ > 0) {
        pace = (measured_to - clock_started_at_ - paused_total_) / questions_asked_;
        pace = std::clamp<Clock::duration>(pace, kMinTurnPace, kMaxTurnPace);
    }
    return static_cast<int>(left / pace);
    //how many more questions fit, at the pace this student has been going. The
    //plan's set questions are ordered early enough to fit in this, rather than
    //left for a last turn the clock never reaches
}

void Session::set_user_id(std::int64_t id) {
    user_id_ = id;
}

std::optional<std::int64_t> Session::user_id() const {
    return user_id_ > 0 ? std::optional<std::int64_t>(user_id_) : std::nullopt;
}

void Session::set_question_limit(int limit) {
    question_limit_ = limit;
}

int Session::question_limit() const {
    return question_limit_;
}

void Session::set_class_id(std::int64_t id) {
    class_id_ = id;
}

std::optional<std::int64_t> Session::class_id() const {
    return class_id_ > 0 ? std::optional<std::int64_t>(class_id_) : std::nullopt;
}

void Session::set_attempt_id(std::int64_t id) {
    attempt_id_ = id;
}

std::int64_t Session::attempt_id() const {
    return attempt_id_;
}

int Session::next_turn_index() {
    return turn_index_++;
}

void Session::record_answer(std::string answer) {
    last_answer_ = std::move(answer);
    //single destination, so the parameter is moved straight in
}

void Session::record_question(std::string question) {
    last_question_ = std::move(question);
    ++questions_asked_;
    //the opinion check used to sit here too. It moved into
    //note_examiner_reply, which Server calls on the same reply a line later,
    //so that one place decides and can say which check found it: split across
    //the two, this one always ran first and left the other with nothing to
    //report even when the examiner had labelled the question itself
}

void Session::note_question_topic(const std::string& topic) {
    const std::string group = topic_group(topic);
    //the tag names a corner of a syllabus topic; the count runs on the topic

    const bool returning =
        std::find(covered_topics_.begin(), covered_topics_.end(), group) !=
        covered_topics_.end();

    if (group.empty() || group == current_topic_ || returning) {
        ++topic_questions_;
        return;
        //an untagged reply, or one still inside the running topic, sits on the
        //thread already running. Two cases had been resetting the count and
        //buying the examiner endless follow-ups: a tag outside the set, and a
        //hop back to a topic already finished. Neither is progress
    }

    if (has_visible_text(current_topic_)) {
        covered_topics_.push_back(current_topic_);
    }

    current_topic_ = group;
    topic_questions_ = 1;
    topic_budget_ = draw_topic_budget(rng_);
}

bool Session::try_begin_job() {
    bool expected = false;
    return job_in_flight_.compare_exchange_strong(expected, true);
    //expected must be a fresh local, compare_exchange overwrites it on failure
}

void Session::end_job() {
    job_in_flight_.store(false);
    //seq_cst store, so this job's writes are visible to the next job's snapshot
}

std::string Session::change_topic_directive() const {
    std::string directive = "You have now asked " +
                            std::to_string(topic_questions_) + " questions ";
    directive += has_visible_text(current_topic_)
                     ? "about \"" + current_topic_ + "\". That topic is finished."
                     : "on the same subject. That subject is finished.";
    directive +=
        " Your next question must open a different topic and must not refer "
        "back to what was just discussed. Ask it straight out, with no "
        "preamble and no linking sentence, and tag it with the new topic.";

    if (!covered_topics_.empty()) {
        directive += " Topics already covered, which you must not return to: ";
        for (std::size_t i = 0; i < covered_topics_.size(); ++i) {
            if (i != 0) {
                directive += ", ";
            }
            directive += covered_topics_[i];
        }
        directive += ".";
        //naming them is what stops the exam circling back once it runs dry
    }

    return directive;
}

bool Session::pending(const RequiredState& state) const {
    return !state.done;
}

std::optional<std::size_t> Session::pick_required(bool changing_topic,
                                                  bool urgent) const {
    const auto find = [this](auto&& wanted) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < required_.size(); ++i) {
            if (pending(required_[i]) && wanted(required_[i].question)) return i;
        }
        return std::nullopt;
    };
    const auto on_current = [this](const PlanQuestion& q) {
        return !q.topic_group.empty() && q.topic_group == current_topic_;
    };
    const auto covered = [this](const std::string& group) {
        return std::find(covered_topics_.begin(), covered_topics_.end(), group) !=
               covered_topics_.end();
    };

    if (urgent) {
        if (auto here = find(on_current)) return here;
        return find([](const PlanQuestion&) { return true; });
    }

    if (changing_topic) {
        if (auto fresh = find([&](const PlanQuestion& q) {
                return !q.topic_group.empty() && q.topic_group != current_topic_ &&
                       !covered(q.topic_group);
            })) {
            return fresh;
            //the best moment for a set question on another topic: the program
            //was about to change topic anyway, and this chooses which to
        }
        if (auto loose = find([](const PlanQuestion& q) { return q.topic_group.empty(); })) {
            return loose;
        }
        return std::nullopt;
        //a set question on a topic already finished waits for the urgent
        //branch rather than dragging the exam back to it early
    }

    return find(on_current);
    //mid-topic, only a set question on the topic already running: anything
    //else would be a topic change the counters did not ask for
}

std::optional<std::string> Session::tense_due(int remaining) const {
    if (!plan_ || plan_->tenses.empty() || questions_asked_ < 2) {
        return std::nullopt;
        //not on the first two questions: the opening is present tense by rule,
        //and the student needs a turn to settle before being steered
    }

    int total = 0;
    std::string most;
    int most_deficit = 0;
    for (const TenseTarget& target : plan_->tenses) {
        int asked = 0;
        for (const auto& [key, count] : tenses_asked_) {
            if (key == target.tense) asked = count;
        }
        const int deficit = std::max(0, target.min_count - asked);
        total += deficit;
        if (deficit > most_deficit) {
            most_deficit = deficit;
            most = target.tense;
        }
    }
    if (total == 0) return std::nullopt;

    const bool running_out = remaining >= 0 && remaining <= total + 1;
    const bool spaced = questions_asked_ - last_tense_order_ >= 3;
    //every third question at most, unless time says otherwise: a tense order
    //every turn would make the exam a grammar drill
    if (!running_out && !spaced) return std::nullopt;
    return most;
}

std::string Session::required_directive(const PlanQuestion& question,
                                        bool opening) const {
    const std::string id = "q" + std::to_string(question.id);
    const bool paraphrase = plan_ && plan_->paraphrase_ok;
    std::string directive;

    if (opening) {
        directive =
            "The student's teacher has set the opening question for this exam. "
            "Ask exactly this and nothing else: \xc2\xab" + question.text +
            "\xc2\xbb.";
    } else {
        directive =
            "The student's teacher has set a question for this exam, and it "
            "must be asked now. React to what the student just said in at most "
            "one short sentence, then ask ";
        directive += paraphrase
                         ? "this question, in your own words if that sounds more "
                           "natural but keeping its meaning: \xc2\xab"
                         : "this question exactly as written: \xc2\xab";
        directive += question.text + "\xc2\xbb. Ask nothing after it.";
    }
    if (!question.topic_group.empty()) {
        directive += " It is about \"" + question.topic_group +
                     "\": tag it with a topic from that group.";
    }
    directive += " Set required_question to \"" + id + "\".";
    return directive;
    //the question is quoted between guillemets so the examiner can see exactly
    //where the teacher's words start and stop, whatever punctuation they carry
}

std::string Session::tense_directive(const std::string& tense) const {
    std::string label = tense;
    std::string example;
    for (const auto& [key, name] : language_->tense_labels) {
        if (key == tense) label = name;
    }
    for (const auto& [key, opening] : language_->tense_examples) {
        if (key == tense) example = opening;
    }

    std::string directive =
        "The student's teacher wants the " + label + " practised. Phrase this "
        "question so that the natural answer uses the " + label;
    if (!example.empty()) {
        directive += ", for example a question shaped like \"" + example + "\"";
    }
    directive +=
        ". Stay on the topic already running, keep it to one short question "
        "and one question mark, and list \"" + tense + "\" in question_tenses.";
    return directive;
}

ReplySchema Session::reply_schema() const {
    ReplySchema schema;
    if (plan_) {
        std::vector<std::string> groups;
        for (const std::string& group : plan_->topics) {
            if (is_topic_group(group)) groups.push_back(group);
        }
        if (!groups.empty()) schema.topic_tags = tags_for_groups(groups);
    }
    if (language_ != nullptr) {
        schema.tenses = language_->tense_labels;
        //asked on every exam, plan or not: a student's own history is worth
        //having whether or not a teacher set anything
    }
    for (const RequiredState& state : required_) {
        if (pending(state)) schema.required_ids.push_back(state.key);
    }
    return schema;
}

Session::ReplyOutcome Session::note_examiner_reply(const std::string& question,
                                                   const ExaminerReply& reply) {
    ReplyOutcome outcome;

    if (!opinion_done_) {
        if (reply.asks_opinion.value_or(false)) {
            outcome.opinion_source = "model";
            //the examiner's own word, which sees an accent or an umlaut the
            //phrase list cannot, and a question that asks for an opinion
            //without reaching for any of the set openings
        } else if (language_ != nullptr &&
                   asks_opinion(question, language_->opinion_openers)) {
            outcome.opinion_source = "openers";
            //an opinion asked without being told to still discharges the debt.
            //Second, not first: the model's label is the better evidence, so
            //when both would fire the record names the better one
        }
        opinion_done_ = outcome.opinion_source.has_value();
    }
    //checked whether or not the plan asked for an opinion question: the
    //directive is what require_opinion switches off, not the record

    std::set<std::string> tenses;
    for (const std::string& key : reply.question_tenses) {
        if (is_tense_key(key)) tenses.insert(key);
    }
    if (language_ != nullptr) {
        for (const std::string& key : detect_tenses(language_->id, question)) {
            tenses.insert(key);
        }
    }
    for (const std::string& key : tenses) {
        for (auto& [target, count] : tenses_asked_) {
            if (target == key) ++count;
        }
        outcome.question_tenses.push_back(key);
    }
    //either source is enough to count towards a target: the model's label and
    //the rules miss different things, and a missed count only means the order
    //is issued once more than it needed to be

    for (RequiredState& state : required_) {
        if (!pending(state)) continue;
        const double score = overlap(state.question.text, question);
        const bool named = reply.required_question == state.key;
        const bool paraphrase = plan_ && plan_->paraphrase_ok;
        const bool asked = score >= kVerbatimMatch ||
                           (named && (paraphrase || score >= kConfirmedMatch));
        if (asked) {
            state.done = true;
            outcome.asked.push_back(state.question.id);
        }
        if (score >= kEvidenceFloor || named) {
            outcome.evidence.push_back(
                ReplyOutcome::QuestionEvidence{state.question.id, score, named});
            //kept whether or not it closed the question. A score just under a
            //threshold is what says the threshold is too high, and a labelled
            //reply that shares almost no words with the question is what says
            //the label cannot be trusted on its own - neither is visible from
            //the verdict alone, which is all that used to be written down
        }
    }
    //every pending question is checked, not only the one ordered: an examiner
    //that asks a set question of its own accord has still asked it

    if (ordered_required_) {
        RequiredState& ordered = required_[*ordered_required_];
        if (!ordered.done && ordered.orders >= kMaxOrders) {
            ordered.done = true;
            outcome.missed.push_back(ordered.question.id);
        }
    }
    ordered_required_.reset();
    return outcome;
}

std::string Session::opinion_directive() const {
    std::string directive =
        "This question must ask the student for an opinion rather than for a "
        "fact. Open it with \"" + language_->opinion_opener_label +
        "\" or an equivalent, and ask what the student thinks";
    //the surrounding instruction stays English, as the rest of the directives
    //here do; only the phrase being quoted back is the exam's own language

    directive += has_visible_text(current_topic_)
                     ? " about \"" + current_topic_ + "\"."
                     : " about what you are discussing.";
    directive +=
        " Stay on the topic already running, keep it to one short clause, and "
        "keep to one question mark. Tag it with the topic as usual.";

    return directive;
}

std::vector<Turn> Session::build_examiner_input() {
    const bool opening_turn = !has_visible_text(last_question_);
    //nothing has been asked yet, so this is the first question of the exam

    std::vector<Turn> input;
    input.reserve(6);
    //at most prompt + anonymity + samples + opinion + question + answer, so
    //one allocation. The opening turn's own user line fits inside the same
    //six: it only appears when question and answer are both absent

    input.push_back(
        Turn{Role::System, opening_turn ? first_prompt_ : ongoing_prompt_});
    //one prompt file or the other, never both. examiner_first.txt is the whole
    //instruction for the opening question and examiner_ongoing.txt is the whole
    //instruction for the rest, so neither can contradict the other
    //copies the prompt into the caller's vector, deliberately: the returned
    //Turns own their text and can outlive this Session's next write

    input.push_back(Turn{Role::System, language_->anonymity_sentence});
    //UNCONDITIONAL, and it carries no slot to fill. This turn used to name the
    //student; it now tells the examiner that it does not know the name and
    //must never ask. The two halves matter equally: nothing identifying leaves
    //for the model, and the model is stopped from eliciting what it was not
    //given - an examiner that asks "come ti chiami?" would put the name into
    //the transcript by the back door, which is the same disclosure one hop
    //later. A second System turn rather than an edit to system_prompt_,
    //because the prompt file on disk is shared by every session

    ordered_required_.reset();
    const bool changing_topic =
        !opening_turn && topic_questions_ >= topic_budget_;

    if (changing_topic) {
        std::vector<std::string> allowed;
        if (plan_) {
            for (const std::string& group : plan_->topics) {
                if (is_topic_group(group)) allowed.push_back(group);
            }
        }
        if (allowed.empty()) allowed = all_topic_groups();
        const bool any_left = std::any_of(
            allowed.begin(), allowed.end(), [this](const std::string& group) {
                return group != current_topic_ &&
                       std::find(covered_topics_.begin(), covered_topics_.end(),
                                 group) == covered_topics_.end();
            });
        if (!any_left) {
            covered_topics_.clear();
            //every topic the exam may cover has had its turn. A plan of two
            //topics gets there in a few minutes, and an order to open a new
            //topic while forbidding every one of them is an order that cannot
            //be obeyed - so the earlier topics are opened up again instead
        }
    }
    const int remaining = opening_turn ? -1 : remaining_turns(Clock::now());

    std::optional<std::size_t> required;
    if (opening_turn) {
        for (std::size_t i = 0; i < required_.size(); ++i) {
            if (pending(required_[i]) && required_[i].question.placement == "opening") {
                required = i;
                break;
            }
        }
    } else {
        const std::size_t open = static_cast<std::size_t>(std::count_if(
            required_.begin(), required_.end(),
            [this](const RequiredState& state) { return pending(state); }));
        const bool urgent = open > 0 && remaining >= 0 &&
                            static_cast<std::size_t>(remaining) <= open;
        //as many set questions left as turns: from here every turn asks one
        required = pick_required(changing_topic, urgent);
    }

    std::optional<std::string> tense;
    if (!opening_turn && !required && !changing_topic) {
        tense = tense_due(remaining);
    }

    const bool opinion_due = opinion_required_ && !opening_turn &&
                             !opinion_done_ && !changing_topic && !required &&
                             !tense && questions_asked_ >= opinion_target_;
    //held back on a turn that already carries an order: one order per turn.
    //It is re-issued every later turn until a reply actually asks an opinion,
    //so an examiner that ignores it once does not lose the question

    if (required) {
        ordered_required_ = required;
        ++required_[*required].orders;
        input.push_back(Turn{Role::System,
                             required_directive(required_[*required].question,
                                                opening_turn)});
        //the teacher's question outranks everything the program would
        //otherwise say this turn, a topic change included: a set question that
        //belongs to another topic is itself the way the topic changes
    } else if (changing_topic) {
        input.push_back(Turn{Role::System, change_topic_directive()});
        //the examiner only ever sees the last exchange, so the count lives here
    } else if (question_bank_) {
        const std::string& group =
            opening_turn ? opening_topic_ : current_topic_;
        std::string examples =
            opening_turn
                ? question_bank_->openers_for(
                      group, kExampleQuestions, rng_,
                      language_->sample_question_header,
                      language_->opening_sample_footer)
                : question_bank_->examples_for(
                      group, kExampleQuestions, rng_,
                      language_->sample_question_header,
                      language_->sample_question_footer);
        if (!examples.empty()) {
            input.push_back(Turn{Role::System, std::move(examples)});
        }
    }
    //never both samples and an order to leave the topic. The samples are for
    //the topic running, and the directive is an order to leave it, so a turn
    //carrying the two would contradict itself. The opening turn samples too,
    //from the group this session's shuffle put first, unless the teacher set
    //the opening question outright

    if (tense) {
        input.push_back(Turn{Role::System, tense_directive(*tense)});
        last_tense_order_ = questions_asked_;
    }

    if (opinion_due) {
        input.push_back(Turn{Role::System, opinion_directive()});
        //alongside the samples, not instead of them: those set the register,
        //this shapes the one question
    }

    if (opening_turn) {
        input.push_back(Turn{Role::Student, language_->opening_turn_text});
        //the exam has to start with a user turn: an examiner backend has only
        //the System prompt at this point, and Gemini rejects a request whose
        //contents array is empty. GeminiExaminer used to synthesise an Italian
        //line of its own here, which put one language inside a backend that
        //should not know about any. It belongs in the pack instead
    }

    if (has_visible_text(last_question_)) {
        input.push_back(Turn{Role::Examiner, last_question_});
    }
    if (has_visible_text(last_answer_)) {
        input.push_back(Turn{Role::Student, last_answer_});
    }
    //strings with no visible characters are skipped rather than sent as blank
    //turns. Only one of these branches runs: opening_turn is defined as having
    //no last_question_, so the opener above never sits beside a real exchange

    return input;
    //by value. NRVO elides the copy, and even if it did not this would move
}

void Session::append_audio(const std::vector<std::int16_t>& chunk) {
    if (audio_buffer_.size() >= kMaxBufferedSamples) {
        return;
    }

    const std::size_t room = kMaxBufferedSamples - audio_buffer_.size();
    const std::size_t take = std::min(room, chunk.size());
    audio_buffer_.insert(audio_buffer_.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(take));
    //keeping earliest 40 seconds of buffer
}

bool Session::audio_full() const {
    return audio_buffer_.size() >= kMaxBufferedSamples;
}

std::vector<std::int16_t> Session::take_audio() {
    std::vector<std::int16_t> result = std::move(audio_buffer_);
    audio_buffer_.clear();
    partial_byte_.clear();
    //the utterance is over, so a half sample left from its last frame belongs
    //to nothing and must not be glued onto the start of the next utterance
    return result;
}

void Session::stash_partial_byte(std::string byte) {
    partial_byte_ = std::move(byte);
}

std::string Session::take_partial_byte() {
    std::string result = std::move(partial_byte_);
    partial_byte_.clear();
    return result;
}


}  // namespace sim
