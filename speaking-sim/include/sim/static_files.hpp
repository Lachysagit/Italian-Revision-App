#pragma once

#include <string>

#include "crow.h"

namespace sim {

crow::response serve_static_file(const std::string& path, const char* content_type);
//whole-file read for the page assets. One function rather than a handler per
//file, which is what the route table used to need

crow::response serve_range_file(const crow::request& req,
                                const std::string& path,
                                const char* content_type);
//adds the Range/206 handling crow does not implement. Without it a browser
//refuses to seek inside an mp3 and the listening page's scrub bar goes dead

bool is_known_language(const std::string& language);
//the listening page carries one folder per language, so the segment naming it is
//held to a fixed list rather than a shape

bool is_safe_clip_name(const std::string& year, const std::string& file);
//year and file arrive from the URL, so they are checked against an allowlist
//before they are ever pasted into a filesystem path

bool is_safe_font_name(const std::string& file);
//same treatment for the font folder: a name from the URL, held to
//<letters, digits, hyphens>.woff2 before it is pasted into a path

bool is_safe_vendor_name(const std::string& file);
//the vendored browser libraries. A closed list rather than a shape: the folder
//is small and every page names its files in full, so a list is one edit to
//extend and nothing else under vendor/ can be read through the route

}  // namespace sim
