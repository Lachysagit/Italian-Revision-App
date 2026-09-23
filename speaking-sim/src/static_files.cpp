#include "sim/static_files.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

namespace sim {

namespace {

constexpr std::int64_t kMaxChunkBytes = 1024 * 1024;
//a browser opens playback with "bytes=0-", which would otherwise mean reading a
//whole clip into memory to answer one request. A short 206 is legal and the
//browser simply asks for the next slice

bool all_digits(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    for (unsigned char character : text) {
        if (!std::isdigit(character)) {
            return false;
        }
    }
    return true;
}

bool parse_range_header(const std::string& header,
                        std::int64_t size,
                        std::int64_t& start,
                        std::int64_t& end) {
    const std::string prefix = "bytes=";
    if (header.rfind(prefix, 0) != 0) {
        return false;
    }

    const std::string spec = header.substr(prefix.size());
    if (spec.find(',') != std::string::npos) {
        return false;
        //multipart ranges are legal but nothing we serve needs them
    }

    const std::size_t dash = spec.find('-');
    if (dash == std::string::npos) {
        return false;
    }

    const std::string first = spec.substr(0, dash);
    const std::string second = spec.substr(dash + 1);

    if (first.empty()) {
        //suffix form: "bytes=-500" asks for the LAST 500 bytes, not the first
        if (!all_digits(second)) {
            return false;
        }
        const std::int64_t wanted = std::stoll(second);
        if (wanted <= 0) {
            return false;
        }
        start = std::max<std::int64_t>(0, size - wanted);
        end = size - 1;
        return true;
    }

    if (!all_digits(first)) {
        return false;
    }
    start = std::stoll(first);

    if (second.empty()) {
        end = size - 1;
    } else if (all_digits(second)) {
        end = std::stoll(second);
    } else {
        return false;
    }
    return true;
}

}  // namespace

crow::response serve_static_file(const std::string& path, const char* content_type) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return crow::response(404, path + " not found");
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    crow::response response(buffer.str());
    response.set_header("Content-Type", content_type);
    return response;
}

bool is_known_language(const std::string& language) {
    return language == "italian" || language == "german";
    //a closed list, so adding a language is a deliberate edit here rather than
    //whatever happens to sit in the listening folder
}

bool is_safe_clip_name(const std::string& year, const std::string& file) {
    if (year.size() != 4 || !all_digits(year)) {
        return false;
    }
    if (file == "familiarisation.mp3") {
        return true;
    }
    //the only other shapes written beside a clip are q<two digits>.mp3 for the audio
    //and q<two digits>.png for a question the exam paper prints as a picture or table
    return file.size() == 7 && file[0] == 'q' && std::isdigit(static_cast<unsigned char>(file[1]))
           && std::isdigit(static_cast<unsigned char>(file[2]))
           && (file.compare(3, 4, ".mp3") == 0 || file.compare(3, 4, ".png") == 0);
}

bool is_safe_font_name(const std::string& file) {
    const std::string suffix = ".woff2";
    if (file.size() <= suffix.size()
        || file.compare(file.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }
    //letters, digits and hyphens are the whole of the names fonts.css asks for,
    //so a dot anywhere but the suffix is already out
    for (unsigned char character : file.substr(0, file.size() - suffix.size())) {
        if (!std::isalnum(character) && character != '-') {
            return false;
        }
    }
    return true;
}

crow::response serve_range_file(const crow::request& req,
                                const std::string& path,
                                const char* content_type) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return crow::response(404);
    }

    const std::int64_t size = static_cast<std::int64_t>(file.tellg());
    if (size <= 0) {
        return crow::response(404);
    }

    const std::string header = req.get_header_value("Range");

    std::int64_t start = 0;
    std::int64_t end = 0;
    if (header.empty() || !parse_range_header(header, size, start, end)) {
        file.seekg(0);
        std::stringstream buffer;
        buffer << file.rdbuf();
        crow::response response(buffer.str());
        response.set_header("Content-Type", content_type);
        response.set_header("Accept-Ranges", "bytes");
        return response;
    }

    if (start >= size || start > end) {
        crow::response response(416);
        response.set_header("Content-Range", "bytes */" + std::to_string(size));
        return response;
    }

    end = std::min(end, size - 1);
    end = std::min(end, start + kMaxChunkBytes - 1);

    const std::int64_t length = end - start + 1;
    std::string body(static_cast<std::size_t>(length), '\0');
    file.seekg(start);
    file.read(&body[0], length);
    body.resize(static_cast<std::size_t>(file.gcount()));

    crow::response response(206, body);
    response.set_header("Content-Type", content_type);
    response.set_header("Accept-Ranges", "bytes");
    response.set_header("Content-Range", "bytes " + std::to_string(start) + "-"
                                             + std::to_string(end) + "/"
                                             + std::to_string(size));
    return response;
}

}  // namespace sim
