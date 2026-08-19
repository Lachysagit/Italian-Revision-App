#include "sim/server.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

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


void Server::run() {
    system_prompt_ = load_system_prompt();
    //load examiner system prompt once at startup

    CROW_ROUTE(app_, "/")
    ([this] {
        return serve_index();
    });

    CROW_WEBSOCKET_ROUTE(app_, "/ws")
        .onopen([this](crow::websocket::connection& conn) {
            auto session = std::make_shared<Session>();
            session->set_system_prompt(system_prompt_);


            //sessions is std::unordered_map 
            //sessions keys are <crow::websocket::connection*, 
            //sessions values are std::shared_ptr<Session>>

            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                sessions_[&conn] = session;
                //for this conn key in the map assign its value the sharedptr session

            }
        })
        .onmessage([this](crow::websocket::connection& conn,
                          const std::string& data,
                          bool is_binary) {
            //params are conn, data, and binary flag
            auto session = find_session(&conn);
            // find session for this conn
            if (!session) {
                return;
            //if nullptr was returned it evaluates to false which returns
            }

            if (is_binary) {
                handle_audio(session, data);
            //binary check
            } else {
                handle_control(conn, session, data);
            }
            //text = JSON control message
        })
        .onclose([this](crow::websocket::connection& conn,
                        const std::string& reason,
                        uint16_t code) {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            sessions_.erase(&conn);
            //erase the conn key in the sessions map
        });

    app_.port(config_.port).multithreaded().run();
}

crow::response Server::serve_index() {
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

std::string Server::load_system_prompt() {
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

std::shared_ptr<Session> Server::find_session(crow::websocket::connection* conn) {
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

}  // namespace sim