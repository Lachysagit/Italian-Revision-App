#include "sim/server.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sim/protocol.hpp"
#include "sim/question_bank.hpp"
#include "sim/static_files.hpp"
#include "sim/text_clean.hpp"
#include "sim/topics.hpp"
#include "sim/translate.hpp"
#include "sim/session.hpp"

namespace sim {

namespace {

// Both are escape hatches, not schedules: the barrier fills in microseconds and
// the handshakes take a few hundred milliseconds. They exist so that a pool
// smaller than worker_threads, or a network that never answers, costs a slow
// first turn instead of a server that never finishes starting.
constexpr int kPrewarmBarrierSeconds = 5;
constexpr int kPrewarmJoinSeconds = 15;

// The translate box is for a sentence at a time. The cap stops an accidental
// paste of the whole transcript turning into one long, billable request.
constexpr std::size_t kMaxTranslateChars = 1000;

// The route only ever serves this app's two directions, so the language pair is
// checked against a list rather than passed through. Without this the request
// body could steer the outbound call at any language Google offers.
bool is_supported_language(const std::string& code) {
    return code == "it" || code == "en" || code == "de";
    //de was missing while the listening page already offered German, so its
    //translate box answered every lookup with an unsupported-pair error
}

crow::response json_error(int status, const std::string& message) {
    crow::json::wvalue json;
    json["error"] = message;
    crow::response response(status, json.dump());
    response.set_header("Content-Type", "application/json");
    return response;
    //an error is JSON too, so the client can read .error the same way on every
    //path instead of guessing whether a body is text or JSON by status code
}

}  // namespace

Server::Server(Config config,
               std::unique_ptr<InterfaceSTT> stt,
               std::unique_ptr<InterfaceExaminer> examiner,
               std::unique_ptr<InterfaceTTS> tts,
               std::unique_ptr<Store> store)
    : config_(std::move(config)),
      stt_(std::move(stt)),
      examiner_(std::move(examiner)),
      tts_(std::move(tts)),
      store_(std::move(store)),
      pool_(config_.worker_threads) {
    //constructor where config_ is initialised
} // constructor


void Server::run() 

    {
    languages_ = LanguageRegistry::load(config_);
    //every language's prompts, bank and voice, read once at startup. Which
    //prompt a turn gets is still Session's decision, because only the Session
    //knows whether a question has been asked yet. load() logs per language and
    //never throws: a missing German prompt file must still leave Italian exams
    //running, the same way a missing prompt file always has

    prewarm_tts();
    prewarm_examiner();
    //before the port is bound, so the first student to connect cannot race them

    CROW_ROUTE(app_, "/") //HTTP ROUTE -----------------------------------
    ([] {
        return serve_static_file("web(frontend)/index.html", "text/html");
    });

    CROW_ROUTE(app_, "/client.js") //HTTP ROUTE -----------------------------------
    ([] {
        return serve_static_file("web(frontend)/client.js", "application/javascript");
    });

    CROW_ROUTE(app_, "/translate.js") //HTTP ROUTE -----------------------------------
    ([] {
        return serve_static_file("web(frontend)/translate.js", "application/javascript");
    });
    //the translate box, shared by both pages

    CROW_ROUTE(app_, "/styles.css") //HTTP ROUTE -----------------------------------
    ([] {
        return serve_static_file("web(frontend)/styles.css", "text/css");
    });

    CROW_ROUTE(app_, "/fonts.css") //HTTP ROUTE -----------------------------------
    ([] {
        return serve_static_file("web(frontend)/fonts.css", "text/css");
    });
    //the @font-face block, linked by both pages

    CROW_ROUTE(app_, "/fonts/<string>") //HTTP ROUTE -----------------------------------
    ([](const std::string& file) {
        if (!is_safe_font_name(file)) {
            return crow::response(404);
        }
        return serve_static_file("web(frontend)/fonts/" + file, "font/woff2");
    });
    //crow's <string> stops at a /, and is_safe_font_name keeps the rest of the
    //folder from being readable through a name the stylesheet never asks for

    CROW_ROUTE(app_, "/listening") //HTTP ROUTE -----------------------------------
    ([] {
        return serve_static_file("web(frontend)/listening.html", "text/html");
    });

    CROW_ROUTE(app_, "/listening.js") //HTTP ROUTE -----------------------------------
    ([] {
        return serve_static_file("web(frontend)/listening.js", "application/javascript");
    });

    CROW_ROUTE(app_, "/listening.css") //HTTP ROUTE -----------------------------------
    ([] {
        return serve_static_file("web(frontend)/listening.css", "text/css");
    });

    CROW_ROUTE(app_, "/listening/<string>/manifest.json") //HTTP ROUTE -----------------------------------
    ([this](const std::string& language) {
        return serve_manifest(language);
    });
    //one clip index per language, re-fetched when the page's picker changes

    CROW_ROUTE(app_, "/listening/<string>/clips/<string>/<string>") //HTTP ROUTE -----------------------------------
    ([this](const crow::request& req, const std::string& language,
            const std::string& year, const std::string& file) {
        return serve_clip(req, language, year, file);
    });
    //crow's <string> stops at a /, so the path cannot be walked upward even
    //before is_safe_clip_name looks at it

    CROW_ROUTE(app_, "/api/gemini-keys") //HTTP ROUTE -----------------------------------
    ([this] {
        return serve_gemini_keys();
    });
    //the settings modal fetches this to populate its picker

    CROW_ROUTE(app_, "/api/languages") //HTTP ROUTE -----------------------------------
    ([this] {
        return serve_languages();
    });
    //and this to populate the language picker beside it

    CROW_ROUTE(app_, "/api/translate").methods("POST"_method) //HTTP ROUTE -----------------------------------
    ([this](const crow::request& req) {
        return serve_translate(req);
    });
    //the translate box posts here. Handlers run on crow's own socket threads,
    //so this blocks one for the call and never touches pool_ - a lookup cannot
    //take a worker away from a student who is mid-turn

    CROW_WEBSOCKET_ROUTE(app_, "/ws") //WEBSOCKET ROUTE ----------------------------------
        .onopen([this](crow::websocket::connection& conn) //handles when websocket is opened
        
            {
            auto session = std::make_shared<Session>();
            session->set_language(&languages_.default_pack());
            //the default until a Start message names one, so a client that
            //never sends a language still runs an exam rather than reaching a
            //null pack on its first turn


            //sessions_ maps crow::websocket::connection* keys to
            //std::shared_ptr<Session> values

            auto handle = std::make_shared<ConnHandle>();
            handle->conn = &conn;
            //one handle per connection instance: a worker still holding the
            //old one sees a nulled conn rather than a new connection

            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                //local variable lock of type lock_guard which calls lock on session_mutex
                sessions_[&conn] = session;
                //for this conn key in the map assign its value the sharedptr session
                conn_handles_[&conn] = std::move(handle);
                //both maps written under the one scope so they cannot drift

            }
            } 
        ) //end of .onopen 

        .onmessage([this](crow::websocket::connection& conn, //handles when websocket receives message
                          const std::string& data,
                          bool is_binary)  //flag
                          
            {
            //params are conn, data, and binary flag
            auto session = find_session(&conn);
            // find session for this conn
            if (!session) {
                return;
            //if nullptr was returned it evaluates to false which returns
            }

            if (is_binary) { //boolean check
                handle_audio(session, data);
            //binary check
            } else { //handles false of boolean if binary
                handle_control(conn, session, data);
            }
            //text = JSON control message
            }
        ) // end of  .onmessage


        .onclose([this](crow::websocket::connection& conn,
                        const std::string& reason,
                        uint16_t code) 
         //params are connection, reason for close and
        //code is a numeric WebSocket close code
        
        {
            std::shared_ptr<ConnHandle> handle;

            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                //lock the mutex
                auto it = conn_handles_.find(&conn);
                if (it != conn_handles_.end()) {
                    handle = std::move(it->second);
                    conn_handles_.erase(it);
                }
                //lifted out before the erase so the handle survives the map entry
                sessions_.erase(&conn);
                //erase the conn key in the sessions map
            } //mutex is unlocked as the lock variable goes out of scope

            //the two scopes are sequential and never nested, deliberately.
            //Holding one while reaching for the other is exactly the deadlock

            CROW_LOG_DEBUG << "websocket closed, code " << code
                           << ", reason: " << reason;
            //reason and code were named and never read. Logging both tells a
            //silent disconnect apart from a client that simply went away

            if (handle) {
                std::lock_guard<std::mutex> lock(handle->m);
                handle->conn = nullptr;
                //blocks until any send in flight releases m; ~Connection
                //cannot begin until this returns, so the send is never cut off
            }
        }
    ); // end of .onclose

    app_.port(config_.port).multithreaded().run(); //IMPORTANT LINE
    //Launches server loop, accepts websocket requests accross multi threads
}

void Server::prewarm_tts() {
    //PiperTTS spawns a fresh piper process per turn, and the first spawn of the
    //run pays for loading the executable, onnxruntime and the voice .onnx off
    //cold disk - seconds, all of it landing on the opening question. Doing it
    //here pulls those pages into the OS file cache while nobody is waiting, so
    //every later spawn is a warm one. The samples are thrown away.
    std::vector<std::string> warmed;
    //each distinct voice once: two languages sharing a voice share its cache
    //entry too, and warming it twice only spends a second spawn on nothing

    for (const LanguagePack* pack : languages_.all()) {
        if (std::find(warmed.begin(), warmed.end(), pack->piper_voice_path) !=
            warmed.end()) {
            continue;
        }
        warmed.push_back(pack->piper_voice_path);

        const auto started = std::chrono::steady_clock::now();
        try {
            const std::vector<std::int16_t> warm =
                tts_->synthesize(pack->prewarm_text, pack->piper_voice_path);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started).count();
            if (warm.empty()) {
                std::cerr << "tts prewarm (" << pack->id << ") produced no audio in "
                          << ms << "ms - piper is not configured, replies will be silent"
                          << std::endl;
            } else {
                std::cerr << "tts prewarm (" << pack->id << "): " << ms << "ms, "
                          << (warm.size() / static_cast<double>(
                                  tts_->sample_rate(pack->piper_voice_path)))
                          << "s of audio discarded" << std::endl;
                //the number to compare against the first turn's tts timing: if they
                //are still close, the cost is synthesis itself, not the cold start
            }
        } catch (const std::exception& e) {
            std::cerr << "tts prewarm (" << pack->id
                      << ") failed, continuing: " << e.what() << std::endl;
            //never fatal, and per voice: a missing German voice must still leave
            //a server that runs Italian exams, the same way a broken piper
            //leaves one that serves the page and the text of every reply
        }
    }
}

void Server::prewarm_examiner() {
    // GeminiExaminer's httplib Client is thread_local, so one prewarm() call
    // would open a connection on ONE pool thread and leave the rest cold. The
    // jobs below hold at a barrier until every one of them is running, which
    // is what guarantees they occupy distinct threads: a job that returned
    // early would free its thread to pick up the next job off the queue and
    // warm the same connection twice.
    const std::size_t count = config_.worker_threads;
    if (count == 0) {
        return;
    }

    struct Barrier {
        std::mutex m;
        std::condition_variable cv;
        std::size_t arrived = 0;
        std::size_t finished = 0;
        bool released = false;
    };
    auto barrier = std::make_shared<Barrier>();
    //shared_ptr rather than stack locals captured by reference: both waits
    //below can time out, and on that path this frame returns while jobs are
    //still running. References would dangle; a shared owner cannot

    for (std::size_t i = 0; i < count; ++i) {
        pool_.enqueue([this, barrier, count] {
            {
                std::unique_lock<std::mutex> lock(barrier->m);
                if (++barrier->arrived == count) {
                    barrier->released = true;
                    barrier->cv.notify_all();
                    //the last job to arrive frees all of them, itself included
                }
                barrier->cv.wait_for(lock,
                                     std::chrono::seconds(kPrewarmBarrierSeconds),
                                     [&] { return barrier->released; });
                //wait_for, not wait: if the pool were ever smaller than
                //worker_threads the barrier could never fill, and a startup
                //that hangs forever is far worse than a cold first turn
            }

            examiner_->prewarm();
            //outside the lock, so all the handshakes overlap instead of
            //queueing one behind another

            {
                std::lock_guard<std::mutex> lock(barrier->m);
                ++barrier->finished;
            }
            barrier->cv.notify_all();
        });
    }

    std::unique_lock<std::mutex> lock(barrier->m);
    barrier->cv.wait_for(lock, std::chrono::seconds(kPrewarmJoinSeconds),
                         [&] { return barrier->finished == count; });
    //waited on rather than abandoned so the port does not open mid-handshake,
    //and so the log below reads in order with the rest of startup
    if (barrier->finished != count) {
        std::cerr << "examiner prewarm: only " << barrier->finished << " of "
                  << count << " workers warmed before the timeout" << std::endl;
        //the stragglers pay the handshake on their first real turn, and the
        //jobs still hold the barrier alive behind us
    }
}

crow::response Server::serve_manifest(const std::string& language)
    {
    if (!is_known_language(language)) {
        return crow::response(404);
    }

    return serve_static_file("listening/" + language + "/manifest.json", "application/json");
}

crow::response Server::serve_clip(const crow::request& req,
                                  const std::string& language,
                                  const std::string& year,
                                  const std::string& file)
    {
    if (!is_known_language(language) || !is_safe_clip_name(year, file)) {
        return crow::response(404);
    }
    //rejected before the path exists, so a rejected name is never opened

    //is_safe_clip_name has already limited this to .mp3 or .png
    const bool is_image = file.compare(file.size() - 4, 4, ".png") == 0;

    return serve_range_file(req, "listening/" + language + "/clips/" + year + "/" + file,
                            is_image ? "image/png" : "audio/mpeg");
}

crow::response Server::serve_gemini_keys()
    {
    crow::json::wvalue json;
    unsigned index = 0;
    //crow wvalue::operator[] takes unsigned, same reasoning as gemini_examiner.cpp
    for (const GeminiKeyOption& option : config_.gemini_api_keys) {
        json[index] = option.name;
        //name only - the key itself never leaves the server
        ++index;
    }
    crow::response response(json.dump());
    response.set_header("Content-Type", "application/json");
    return response;
}

crow::response Server::serve_languages()
    {
    crow::json::wvalue json;
    unsigned index = 0;
    for (const LanguagePack* pack : languages_.all()) {
        json[index]["id"] = pack->id;
        json[index]["label"] = pack->display_name;
        json[index]["translate_code"] = pack->translate_code;
        //objects rather than bare strings: the picker needs the wire value, the
        //text to show and the code the translate box switches to, and deriving
        //any of the three in JavaScript would be a second table to keep in step
        ++index;
    }
    crow::response response(json.dump());
    response.set_header("Content-Type", "application/json");
    return response;
}

crow::response Server::serve_translate(const crow::request& req)
    {
    if (config_.translate_api_key.empty()) {
        return json_error(503, "Translation is not configured on this server.");
        //said out loud rather than attempted with an empty key, which would come
        //back as an opaque 403 from Google and read like a broken feature
    }

    crow::json::rvalue parsed = crow::json::load(req.body);
    if (!parsed) {
        return json_error(400, "Request body was not valid JSON.");
    }

    if (!parsed.has("text") || parsed["text"].t() != crow::json::type::String ||
        !parsed.has("source") || parsed["source"].t() != crow::json::type::String ||
        !parsed.has("target") || parsed["target"].t() != crow::json::type::String) {
        return json_error(400, "Expected text, source and target strings.");
        //both the presence and the type are checked, same as protocol.cpp does:
        //rvalue::operator[] throws on a missing key and .s() on a wrong type
    }

    const std::string text(parsed["text"].s());
    const std::string source(parsed["source"].s());
    const std::string target(parsed["target"].s());

    if (text.empty()) {
        return json_error(400, "Nothing to translate.");
    }
    if (text.size() > kMaxTranslateChars) {
        return json_error(400, "That is too long - translate a sentence at a time.");
    }
    if (!is_supported_language(source) || !is_supported_language(target) ||
        source == target) {
        return json_error(400, "Unsupported language pair.");
    }

    try {
        const std::string translation =
            translate_text(config_.translate_api_key, text, source, target);

        crow::json::wvalue json;
        json["translation"] = translation;
        crow::response response(json.dump());
        response.set_header("Content-Type", "application/json");
        return response;
    } catch (const std::exception& e) {
        std::cerr << "translate route: " << e.what() << std::endl;
        return json_error(502, "The translation service could not be reached.");
        //the detail goes to the operator's log and a fixed string to the page,
        //the same split the examiner path uses for its failures
    }
}

std::shared_ptr<Session> Server::find_session(crow::websocket::connection* conn) 

    {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    //lock the map of sessions_
    auto it = sessions_.find(conn);
    //.find retursn an iterator pointing at the found entry fo conn
    // or sessions_.end() if not found
    if (it == sessions_.end()) {
    //evaluate if no conn found
        return nullptr;
    //return a nullptr because the return value is still a shared_ptr object
    }
    return it->second;
    //second is the value not the key in the map
    //return the shared_ptr value of session for this conn
    }

std::shared_ptr<ConnHandle> Server::find_conn_handle(crow::websocket::connection* conn)

    {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    //a map lookup and nothing more, same as find_session
    auto it = conn_handles_.find(conn);
    if (it == conn_handles_.end()) {
        return nullptr;
        //onclose already ran for this connection, so there is nothing to send on
    }
    return it->second;
    //the caller keeps this shared_ptr alive for as long as the job lives, so the
    //handle outlives the connection even if it is destroyed mid-turn
    }

    
void Server::handle_audio(const std::shared_ptr<Session>& session,
                          const std::string& data) 
                          
    { 
    //passed in session and std::string of PCM bytes
    std::string bytes = session->take_partial_byte();
    bytes += data;
    //a websocket frame is free to split a 16-bit sample across two frames.
    //the odd byte left over last time is put back on the front of the new buffer

    const std::size_t count = bytes.size() / sizeof(std::int16_t);
    //count is the number of samples: total bytes / bytes per sample,
    //i.e. size in bytes / sizeof(std::int16_t)

    std::vector<std::int16_t> pcm(count);
    if (count > 0) {
        std::memcpy(pcm.data(), bytes.data(), count * sizeof(std::int16_t));
        //memcpy rather than reinterpret_cast: bytes.data() is a char* with
        //no 2-byte alignment guarantee, so reading it as std::int16_t* is UB
    }
    //a lone odd byte leaves count at 0, and memcpy from the possibly-null
    //data() of an empty vector is undefined even for length 0

    if (bytes.size() % sizeof(std::int16_t) != 0) {
        session->stash_partial_byte(bytes.substr(count * sizeof(std::int16_t)));
        //hold the trailing half sample back for the next frame
    }

    const bool was_full = session->audio_full();
    session->append_audio(pcm);
    //pcm now has its own heap allocated memory which is a vector of 16 bit int

    if (!was_full && session->audio_full()) {
        CROW_LOG_WARNING << "session audio buffer hit its "
                         << (Session::kMaxBufferedSamples / Session::kCaptureSampleRate)
                         << "s cap, further audio is dropped until Stop";
        //logged on the transition only, otherwise a client that keeps streaming
        //past the cap would produce a warning per frame for as long as it runs
    }
}

void Server::handle_control(crow::websocket::connection& conn,
                            const std::shared_ptr<Session>& session,
                            const std::string& data)

    {
    const crow::json::rvalue parsed = crow::json::load(data);
    //parse the text status data into a CROS::JSON rvalue

    if (!parsed) {     // ignore if malformed
        return;
    }


    const Message message = from_json(parsed); //runs function in protocol.cpp
    //convert the readable JSON into a Message object

    if (message.type == MessageType::Start) {
        std::shared_ptr<ConnHandle> handle = find_conn_handle(&conn);
        if (!handle) {
            return;
        } //the connection is already closing, so there is nowhere to send a reply

        if (!session->try_begin_job()) {
            send_busy(handle);
            return;
        } //a job is already in flight on this session, so refuse this message

        if (const LanguagePack* pack = languages_.find(message.language)) {
            session->set_language(pack);
        }
        //an unknown or absent language leaves the default set on open, rather
        //than failing the Start: a stale client or a typo must still get an
        //exam. Set before the job is enqueued, because build_examiner_input()
        //reads the pack's prompts the moment the opening question is queued

        session->set_gemini_key_name(message.gemini_key);
        session->set_student_name(message.student_name);
        //all three picked once, before the first job, and reused by every later
        //turn - Stop messages carry none of these fields of their own. Set
        //before the job is enqueued, so even the opening question knows them

        std::shared_ptr<Session> claim(session.get(), [session](Session* s) { s->end_job(); });
        //not an owner, just an RAII handle whose deleter releases the claim
        //the deleter holds session, so the Session outlives the end_job() call

        enqueue_pipeline_job(std::move(handle), session, {}, false, std::move(claim));
        return;
    }

    if (message.type != MessageType::Stop) {
        return;
    } 

    std::shared_ptr<ConnHandle> handle = find_conn_handle(&conn);
    if (!handle) {
        return;
    } //resolved before try_begin_job(), so a connection that is already closing

    if (!session->try_begin_job()) {
        send_busy(handle);
        return;
    } //refuse before take_audio(), so a rejected Stop does not discard the buffer

    std::shared_ptr<Session> claim(session.get(), [session](Session* s) { s->end_job(); });

    std::vector<std::int16_t> utterance_audio = session->take_audio();
    //take_audio() returns the completed audio buffer, clearing the session buffer
    //taking the audio on the socket thread to seperate it from any new incoming audio\

    enqueue_pipeline_job(std::move(handle), session, std::move(utterance_audio), true,
                         std::move(claim), message.final);
    //the handle rather than &conn: the job outlives handle_control, and by
    //then the raw pointer may name a destroyed connection. message.final is
    //the browser saying its clock has run out, which turns this into the
    //last job of the session: transcribed, but never sent to the examiner
}

void Server::enqueue_pipeline_job(std::shared_ptr<ConnHandle> handle,
                                  const std::shared_ptr<Session>& session,
                                  std::vector<std::int16_t> utterance_audio,
                                  bool transcribe_first,
                                  std::shared_ptr<Session> claim,
                                  bool answer_only)
                                  {
    std::vector<Turn> examiner_input = session->build_examiner_input();
    //still on Crow's socket thread, which the claim has already made exclusive

    const LanguagePack* language = &session->language();
    //snapshotted here alongside the input, for the same reason: the socket
    //thread is the one that knows the session is not mid-change. The pack
    //itself outlives every session, so the pointer stays good in the worker

    if (transcribe_first && !examiner_input.empty() &&
        examiner_input.back().role == Role::Student) {
        examiner_input.pop_back();
        //drops the previous turn's answer, this job appends a fresher one below
    }

    pool_.enqueue([this, session, transcribe_first, answer_only, language,
        handle = std::move(handle),
        job_audio = std::move(utterance_audio),
        job_input = std::move(examiner_input),
        claim = std::move(claim)]() mutable

    //handle is captured by value

    {
        std::string reply;
        std::vector<std::int16_t> speech;
        bool text_sent = false;
        //once the question has gone out promising audio, the client is muted
        //until a binary frame arrives, so every exit below has to send one

        using clock = std::chrono::steady_clock;
        const auto ms_since = [](clock::time_point t) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       clock::now() - t).count();
        };
        const auto turn_started = clock::now();
        long long stt_ms = 0;
        long long examiner_ms = 0;
        long long tts_ms = 0;
        //stage timings on stderr, so a slow turn names one backend. Declared
        //out here so the send below still runs after a failure

        try {
            if (transcribe_first) {
                const auto stt_started = clock::now();
                std::string transcript =
                    stt_->transcribe(job_audio, language->whisper_code);
                stt_ms = ms_since(stt_started);

                if (!transcript.empty()) {
                    send_transcript(handle, transcript);
                    //paint what STT heard before the reply, so a misheard
                    //answer is visible rather than only a reply that makes no sense

                    job_input.push_back(Turn{Role::Student, transcript});
                    //onto the owned snapshot, STT had not run when it was built

                    session->record_answer(std::move(transcript));
                    //write-through so the NEXT turn's snapshot can see this answer
                } else {
                    std::cerr << "turn skipped: empty transcript, no examiner "
                                 "request made (audio "
                              << (job_audio.size() / 16000.0) << "s, stt "
                              << stt_ms << "ms)\n";

                    send_error(handle, "didn't catch that, please try again");
                    if (answer_only) {
                        send_status(handle, "ended");
                        //nothing to re-arm for: the clock has run out and this
                        //job was the last one, heard or not
                    } else {
                        send_examiner_text(handle, reply, false, 0);
                    }
                    return;
                    //no audio promised, so the client re-arms off the text alone
                    //an empty transcript never reaches the examiner: respond()
                }
            }

            if (answer_only) {
                send_status(handle, "ended");
                return;
                //the exam clock ran out before this answer was submitted. It
                //has been transcribed, painted and recorded above, and the job
                //stops here rather than spending a question the student has no
                //time left to hear
            }

            const auto examiner_started = clock::now();
            const std::string raw =
                examiner_->respond(job_input, session->gemini_key_name());
            examiner_ms = ms_since(examiner_started);

            const std::string topic = topic_tag(raw);
            const std::string group = topic_group(topic);
            //logged only; Session maps the tag itself
            reply = clean_for_speech(raw);
            //read then stripped, so the tag never reaches the student or piper
            //cleaned here, once, ahead of all three consumers below: the model
            //slips into markdown however plainly the prompt asks for prose, and
            //the markers reached the transcript and piper's mouth alike
            //borrows the lambda's own vector, which nothing else can touch

            session->record_question(reply);
            session->note_question_topic(topic);

            send_examiner_text(handle, reply, true,
                               tts_->sample_rate(language->piper_voice_path));
            text_sent = true;
            //ahead of synthesis, not after it. The question is on screen while
            //piper is still working, so the wait the student actually sees is
            //the examiner call alone rather than examiner + tts back to back

            const auto tts_started = clock::now();
            speech = tts_->synthesize(reply, language->piper_voice_path);
            tts_ms = ms_since(tts_started);

            std::cerr << "turn timings: audio " << (job_audio.size() / 16000.0)
                      << "s, stt " << stt_ms << "ms, examiner " << examiner_ms
                      << "ms, tts " << tts_ms << "ms, total "
                      << ms_since(turn_started) << "ms, topic "
                      << (topic.empty() ? "(untagged)" : topic) << ", group "
                      << (group.empty() ? "(unrecognised)" : group) << "\n";
            //an exam full of "(untagged)" means the budget is running blind,
            //and "(unrecognised)" means a tag arrived from outside the enum
            //16000 is the capture rate the client resamples to, and the rate
            //whisper requires it
        } catch (const std::exception& e) {
            std::cerr << "turn failed, sending what we have: " << e.what() << '\n';
            speech.clear();
            send_error(handle, "something went wrong on that turn");
            //a fixed student-facing string, 
        } catch (...) {
            std::cerr << "turn failed with non-std exception, sending what we have\n";
            speech.clear();
            send_error(handle, "something went wrong on that turn");
            //same recovery, and reply is preserved for the same reason as above
        }

        if (text_sent) {
            send_speech(handle, speech);
            //empty if synthesis threw, which is the signal that re-arms the mic
        } else {
            send_examiner_text(handle, reply, false, 0);
            //the examiner itself failed, so reply is empty and no audio is owed
            //rate 0 because no frame follows; send_examiner_text writes the
            //field only when it is positive
        }
        //hand the result back to Crow's thread for sending. 
    }
    );
}

void Server::send_text_on_handle(const std::shared_ptr<ConnHandle>& handle,
                                 const std::string& json) {
    std::lock_guard<std::mutex> lock(handle->m);
    //same discipline as send_speech: the null check and the send are

    crow::websocket::connection* conn_ptr = handle->conn;
    if (conn_ptr == nullptr) {
        return;
    }
    conn_ptr->send_text(json);
}

void Server::send_status(const std::shared_ptr<ConnHandle>& handle,
                         const std::string& text) {
    Message message;
    message.type = MessageType::Status;
    message.payload = text;
    send_text_on_handle(handle, to_json(message).dump());
    //carries no sample_rate, so like send_error it paints nothing: the client
    //reads the payload and acts on it
}

void Server::send_busy(const std::shared_ptr<ConnHandle>& handle) {
    send_status(handle, "busy");
}

void Server::send_error(const std::shared_ptr<ConnHandle>& handle,
                        const std::string& text) {
    Message message;
    message.type = MessageType::Error;
    message.payload = text;
    send_text_on_handle(handle, to_json(message).dump());
    //carries no sample_rate, so it cannot be mistaken for a turn. It lands in
    //client.js addLog rather than addTurn, painting nothing in #transcript
}

void Server::send_transcript(const std::shared_ptr<ConnHandle>& handle,
                             const std::string& text) {
    if (text.empty()) {
        return;
        //an empty transcript is not a turn, so nothing is painted for it
    }
    Message message;
    message.type = MessageType::Transcript;
    message.payload = text;
    send_text_on_handle(handle, to_json(message).dump());
    //MessageType::Transcript was never constructed, so the client branch was
}

void Server::send_examiner_text(const std::shared_ptr<ConnHandle>& handle,
                                const std::string& reply,
                                bool speech_follows,
                                int sample_rate) {
    Message message; //create Message Object
    message.type = MessageType::ExaminerText; //Set Message.type to Examiner Text
    message.payload = reply; //set payload to examiners reply
    if (speech_follows) {
        message.sample_rate = sample_rate;
        //tell the browser what rate the PCM frame that follows was produced at.
        //Taken from the backend rather than from the samples, because this is
        //now sent BEFORE synthesis runs - there are no samples to measure yet.
        //Its presence is also how the client knows to hold the mic muted until
        //the binary frame lands, so it must not be set on a text-only turn
        //passed in rather than read from tts_ here: the rate belongs to the
        //session's voice, and this function has no business knowing which
        //voice that is. Two languages on one server report different rates
    }
    send_text_on_handle(handle, to_json(message).dump());
}

void Server::send_speech(const std::shared_ptr<ConnHandle>& handle,
                         const std::vector<std::int16_t>& speech) {
    std::lock_guard<std::mutex> lock(handle->m);
    //the connection own mutex, not sessions_mutex_: it stops onclose letting
    //~Connection run mid-write without stalling every other connection

    crow::websocket::connection* conn_ptr = handle->conn;
    if (conn_ptr == nullptr) {
        return;
        //onclose already ran, the connection is gone and the turn is dropped.
        //Nothing to re-arm: the client that owned it is no longer listening
    }

    if (speech.empty()) {
        conn_ptr->send_binary(std::string());
        //a zero length frame, deliberately, not nothing. playAudio treats a
        //length of 0 as "no audio to wait for" and hands the turn back to the
        //student, which is the only way out once the text promised audio
        return;
    }

    const char* bytes = reinterpret_cast<const char*>(speech.data());
    //speech.data() returns a const std::int16_t* to sample 0 of the audio
    //reinterpret that pointer as a const char* so the samples are viewed as raw bytes

    const std::size_t byte_count = speech.size() * sizeof(std::int16_t);
    //byte_count is the total number of bytes in the audio
    //number of bytes = number of samples * bytes per sample

    conn_ptr->send_binary(std::string(bytes, byte_count));
    //a std::string built from the byte range as a container, not text,
    //then sent down the socket as a binary frame
}
} // namespace sim

