#include "sim/server.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
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
      pool_(2),
      stt_(std::move(stt)),
      examiner_(std::move(examiner)),
      tts_(std::move(tts)) {
}


void Server::run() 

    {
    system_prompt_ = load_system_prompt();
    //load examiner system prompt once at startup

    CROW_ROUTE(app_, "/") //HTTP ROUTE -----------------------------------
    ([this] {
        return serve_index();
    });

    CROW_ROUTE(app_, "/client.js") //HTTP ROUTE -----------------------------------
    ([this] {
        return serve_client_script();
    });
    //index.html pulls this in with <script src="client.js">. Without a route
    //for it the page renders and the browser client never runs at all

    CROW_WEBSOCKET_ROUTE(app_, "/ws") //WEBSOCKET ROUTE ----------------------------------
        .onopen([this](crow::websocket::connection& conn) 
        
            {
            auto session = std::make_shared<Session>();
            session->set_system_prompt(system_prompt_);


            //sessions is std::unordered_map 
            //sessions keys are <crow::websocket::connection*, 
            //sessions values are std::shared_ptr<Session>>

            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                //local variable lock of type lock_guard which calls lock on session_mutex
                sessions_[&conn] = session;
                //for this conn key in the map assign its value the sharedptr session

            }
            } 
        ) //end of .onopen

        .onmessage([this](crow::websocket::connection& conn,
                          const std::string& data,
                          bool is_binary) 
                          
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
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            //lock the mutex
            sessions_.erase(&conn);
            //erase the conn key in the sessions map
        } //mutex is unlocked as the lock variable goes out of scope
    ); // end of .onclose

    app_.port(config_.port).multithreaded().run();
}

crow::response Server::serve_index() 

    {
    std::ifstream file("web/index.html");
    if (!file) {
        return crow::response(404, "index.html not found");
    }
    //open index html file

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

std::string Server::load_system_prompt() 
    
    {
    std::ifstream file("prompts/examiner_system.txt");
    if (!file) {
        return "You are an examiner. Ask the student questions.";
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
    
void Server::handle_audio(const std::shared_ptr<Session>& session,
                          const std::string& data) 
                          
    { 
    //passed in session and std::string of PCM bytes
    std::string bytes = session->take_partial_byte();
    bytes += data;
    //a websocket frame is free to split a 16-bit sample across two frames.
    //the odd byte left over last time is put back on the front here, otherwise
    //it is dropped and every following sample is shifted by one byte

    const std::size_t count = bytes.size() / sizeof(std::int16_t);
    //count is the amount of samples in the audio data
    //number of samples = total bytes / bytes per sample OR
    //number of samples = audio data (bytes) / size of 16bit int (2bytes)

    std::vector<std::int16_t> pcm(count);
    std::memcpy(pcm.data(), bytes.data(), count * sizeof(std::int16_t));
    //memcpy rather than reinterpret_cast: bytes.data() is a char* with no
    //guarantee of 2-byte alignment, and reading it as std::int16_t* is
    //undefined behaviour on targets that care

    if (bytes.size() % sizeof(std::int16_t) != 0) {
        session->stash_partial_byte(bytes.substr(count * sizeof(std::int16_t)));
        //hold the trailing half sample back for the next frame
    }

    session->append_audio(pcm);
    //pcm now has its own heap allocated memory which is a vector of 16 bit int
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
        enqueue_pipeline_job(&conn, session, {}, false);
        //the opening turn: no audio has been spoken yet, so there is nothing to
        //transcribe. The examiner runs on the system prompt alone and asks the
        //first question, instead of the student having to talk into silence
        return;
    }

    if (message.type != MessageType::Stop) {
        return;
    } // only for handing stop

    std::vector<std::int16_t> utterance_audio = session->take_audio();
    //take_audio() returns the completed audio buffer, clearing the session buffer
    //taking the audio on the socket thread to seperate it from any new incoming audio\

    crow::websocket::connection* conn_ptr = &conn; //CRUCIAL
    //store a local variable reference to the connection
    //Because this variable is then used on a Worker Thread after handle_contorl returns

    enqueue_pipeline_job(conn_ptr, session, std::move(utterance_audio), true);
    //hand the audio and connection to the pipeline job builder
}

void Server::enqueue_pipeline_job(crow::websocket::connection* conn_ptr,
                                  const std::shared_ptr<Session>& session,
                                  std::vector<std::int16_t> utterance_audio,
                                  bool transcribe_first) 
                                  {
    pool_.enqueue([this, session, conn_ptr, transcribe_first,
        job_audio = std::move(utterance_audio)]

    //this captures the server Object to access member functions
    //session paramater is shared ptr to Session object to utilise its functions
    //create a new variable called audio which has the utterance_audio moved into it
    //therefore the audio is moved not duplicated which is expensive
    //transcribe_first is false for the opening turn, where the student has not
    //spoken yet and there is no audio to run through STT

    {
        if (transcribe_first) {
            const std::string transcript = stt_->transcribe(job_audio);
            session->record_answer(transcript);
        }

        const std::string reply = examiner_->respond(session->get_history());
        session->record_question(reply);

        const std::vector<std::int16_t> speech = tts_->synthesize(reply);

        send_examiner_result(conn_ptr, reply, speech);
        //hand the result back to Crow's thread for sending
    }
    );
}

void Server::send_examiner_result(crow::websocket::connection* conn_ptr,
                                  const std::string& reply,
                                  const std::vector<std::int16_t>& speech) {
    Message message; //create Message Object
    message.type = MessageType::ExaminerText; //Set Message.type to Examiner Text
    message.payload = reply; //set payload to examiners reply
    message.sample_rate = tts_->sample_rate();
    //tell the browser what rate the PCM frame that follows was produced at,
    //otherwise it plays it back at the AudioContext rate and the pitch shifts
    const std::string json = to_json(message).dump();
    //build the examiner text as a CROW::JSON object

    conn_ptr->send_text(json);
    //send the reply text down the socket as a text frame
    //Crow handles the thread-safety of the send internally

    const char* bytes = reinterpret_cast<const char*>(speech.data());
    //speech.data() returns a const std::int16_t* to sample 0 of the audio
    //reinterpret that pointer as a const char* so the samples are viewed as raw bytes

    const std::size_t byte_count = speech.size() * sizeof(std::int16_t);
    //byte_count is the total number of bytes in the audio
    //number of bytes = number of samples * bytes per sample OR

    conn_ptr->send_binary(std::string(bytes, byte_count));
    //construct a std::string from the byte range (start pointer + length)
    //the string here is just a byte container, not readable text
    //send those raw PCM bytes down the socket as a binary frame
}
} // namespace sim

