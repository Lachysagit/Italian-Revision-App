#pragma once

#include <cctype>
#include <cstddef>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace sim {

// The small string helpers that had grown a private copy in most files: three
// trims, six lowercase loops, three comma splitters and four whole-file reads,
// each spelled slightly differently. Header-only and free of every dependency,
// so the safety tests - which link nothing but the standard library - can use
// them too.

inline constexpr std::string_view kWhitespace = " \t\r\n";
//\r included: the prompt files and .env are edited on Windows

inline std::string trim(std::string_view text,
                        std::string_view strip = kWhitespace) {
    const std::size_t begin = text.find_first_not_of(strip);
    if (begin == std::string_view::npos) return std::string();
    const std::size_t end = text.find_last_not_of(strip);
    return std::string(text.substr(begin, end - begin + 1));
}

// ASCII only, byte by byte. A multi-byte UTF-8 letter passes through untouched,
// which is why every list compared against this (opinion openers, email
// addresses, header names) has to be ASCII itself.
inline std::string to_lower_ascii(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        //the unsigned char cast is required: tolower on a negative char is UB
    }
    return text;
}

// "a, b ,,c" -> {"a", "b", "c"}: split on one separator, each item trimmed,
// empty items dropped. The shape every hand-typed list in .env has.
inline std::vector<std::string> split_list(std::string_view text, char separator) {
    std::vector<std::string> items;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t at = text.find(separator, start);
        const std::size_t end = at == std::string_view::npos ? text.size() : at;
        std::string item = trim(text.substr(start, end - start));
        if (!item.empty()) items.push_back(std::move(item));
        if (at == std::string_view::npos) break;
        start = at + 1;
    }
    return items;
}

inline std::string join(const std::vector<std::string>& items,
                        std::string_view separator) {
    std::string out;
    for (const std::string& item : items) {
        if (!out.empty()) out += separator;
        out += item;
    }
    return out;
}

// The whole file, or nullopt when it cannot be opened. Binary by default, so
// a font or an mp3 comes back byte for byte; pass std::ios::in for a text file
// whose Windows line endings should be folded to \n, as the prompts are.
inline std::optional<std::string> read_file(
    const std::string& path, std::ios::openmode mode = std::ios::binary) {
    std::ifstream file(path, mode);
    if (!file) return std::nullopt;
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

}  // namespace sim
