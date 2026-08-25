#include "sim/server.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sim/protocol.hpp"
#include "sim/session.hpp"

namespace sim {

Server::Server(Config config,
               std::unique_ptr<InterfaceSTT> stt,
               std::unique_ptr<InterfaceExaminer> examiner,
               std::unique_ptr<InterfaceTTS> tts)
    : config_(std::move(config)),
      stt_(std::move(stt)),
      examiner_(std::move(examiner)),
      tts_(std::move(tts)),
      pool_(config_.worker_threads) {
    //constructor where config_ is initialised
} // constructor


void Server::run() 

    {
    system_prompt_ = load_system_prompt();
    //load examiner system prompt once at startup, file open

    CROW_ROUTE(app_, "/") //HTTP ROUTE -----------------------------------
    ([this] {
        return serve_index();
    });

    CROW_ROUTE(app_, "/client.js") //HTTP ROUTE -----------------------------------
    ([this] {
        return serve_client_script();
    });
    //index.html pulls this in with <script src="client.js">

    CROW_ROUTE(app_, "/styles.css") //HTTP ROUTE -----------------------------------
    ([this] {
        return serve_stylesheet();
    });
    //same reason as client.js: index.html links it

    CROW_WEBSOCKET_ROUTE(app_, "/ws") //WEBSOCKET ROUTE ----------------------------------
        .onopen([this](crow::websocket::connection& conn) //handles when websocket is opened
        
            {
            auto session = std::make_shared<Session>();
            session->set_system_prompt(system_prompt_);


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

crow::response Server::serve_index() 
    {
    std::ifstream file("web(frontend)/index.html");
    if (!file) {
        return crow::response(404, "index.html not found");
    }
    //open index html file with error handling

    std::stringstream buffer;
    buffer << file.rdbuf();
    //stream entire index file into a string
    crow::response response(buffer.str());
    //build http response
    response.set_header("Content-Type", "text/html");
    //set header so its rendered as webpage
    return response;
}

crow::response Server::serve_client_script() 

    {
    std::ifstream file("web/client.js");
    if (!file) {
        return crow::response(404, "client.js not found");
    }
    //open the browser client script

    std::stringstream buffer;
    buffer << file.rdbuf();
    //stream the entire script into a string
    crow::response response(buffer.str());
    //build http response
    response.set_header("Content-Type", "application/javascript");
    //set header so the browser executes it rather than displaying it
    return response;
}

crow::response Server::serve_stylesheet() 

    {
    std::ifstream file("web/styles.css");
    if (!file) {
        return crow::response(404, "styles.css not found");
    }
    //open the page stylesheet

    std::stringstream buffer;
    buffer << file.rdbuf();
    //stream the entire stylesheet into a string
    crow::response response(buffer.str());
    //build http response
    response.set_header("Content-Type", "text/css");
    //set header so the browser applies it rather than displaying it as text
    return response;
}

std::string Server::load_system_prompt() 
    
    {
    std::ifstream file("prompts/examiner_system.txt");
    if (!file) {
        return "You are an examiner. Ask the student questions in Italian at a begginers level.";
    }
    //open examiner system prompt file, fallback if missing

    std::stringstream buffer;
    buffer << file.rdbuf();
    //stream entire prompt file into a string
    return buffer.str();
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

    enqueue_pipeline_job(std::move(handle), session, std::move(utterance_audio), true, std::move(claim));
    //the handle rather than &conn: the job outlives handle_control, and by
    //then the raw pointer may name a destroyed connection
}

void Server::enqueue_pipeline_job(std::shared_ptr<ConnHandle> handle,
                                  const std::shared_ptr<Session>& session,
                                  std::vector<std::int16_t> utterance_audio,
                                  bool transcribe_first,
                                  std::shared_ptr<Session> claim)
                                  {
    std::vector<Turn> examiner_input = session->build_examiner_input();
    //still on Crow's socket thread, which the claim has already made exclusive

    if (transcribe_first && !examiner_input.empty() &&
        examiner_input.back().role == Role::Student) {
        examiner_input.pop_back();
        //drops the previous turn's answer, this job appends a fresher one below
    }

    pool_.enqueue([this, session, transcribe_first,
        handle = std::move(handle),
        job_audio = std::move(utterance_audio),
        job_input = std::move(examiner_input),
        claim = std::move(claim)]() mutable

    //handle is captured by value

    {
        std::string reply;
        std::vector<std::int16_t> speech;

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
                std::string transcript = stt_->transcribe(job_audio);
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
                    send_examiner_result(handle, reply, speech);
                    return;
                    //an empty transcript never reaches the examiner: respond()
                }
            }

            const auto examiner_started = clock::now();
            reply = examiner_->respond(job_input);
            examiner_ms = ms_since(examiner_started);
            //borrows the lambda's own vector, which nothing else can touch

            session->record_question(reply);

            const auto tts_started = clock::now();
            speech = tts_->synthesize(reply);
            tts_ms = ms_since(tts_started);

            std::cerr << "turn timings: audio " << (job_audio.size() / 16000.0)
                      << "s, stt " << stt_ms << "ms, examiner " << examiner_ms
                      << "ms, tts " << tts_ms << "ms, total "
                      << ms_since(turn_started) << "ms\n";
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

        send_examiner_result(handle, reply, speech);
        //hand the result back to Crow's thread for sending. 
    }
    );
}

void Server::send_text_on_handle(const std::shared_ptr<ConnHandle>& handle,
                                 const std::string& json) {
    std::lock_guard<std::mutex> lock(handle->m);
    //same discipline as send_examiner_result: the null check and the send are

    crow::websocket::connection* conn_ptr = handle->conn;
    if (conn_ptr == nullptr) {
        return;
    }
    conn_ptr->send_text(json);
}

void Server::send_busy(const std::shared_ptr<ConnHandle>& handle) {
    Message message;
    message.type = MessageType::Status;
    message.payload = "busy";
    send_text_on_handle(handle, to_json(message).dump());
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

void Server::send_examiner_result(const std::shared_ptr<ConnHandle>& handle,
                                  const std::string& reply,
                                  const std::vector<std::int16_t>& speech) {
    Message message; //create Message Object
    message.type = MessageType::ExaminerText; //Set Message.type to Examiner Text
    message.payload = reply; //set payload to examiners reply
    if (!speech.empty()) {
        message.sample_rate = tts_->sample_rate();
        //tell the browser what rate the PCM frame that follows was produced at,
    }
    
    const std::string json = to_json(message).dump();
    //build the examiner text as a CROW::JSON object

    std::lock_guard<std::mutex> lock(handle->m);
    //the connection's own mutex, not sessions_mutex_: it stops onclose letting
    //~Connection run mid-write without stalling every other connection

    crow::websocket::connection* conn_ptr = handle->conn;
    if (conn_ptr == nullptr) {
        return;
        //onclose already ran, the connection is gone and the turn is dropped.
        //Nothing to re-arm: the client that owned it is no longer listening
    }

    conn_ptr->send_text(json);
    //send the reply text down the socket as a text frame

    if (!speech.empty()) {
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
    //Currently no handling for empty audio buffer
}
} // namespace sim

