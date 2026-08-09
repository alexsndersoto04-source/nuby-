#pragma once

#include "../core/string_utils.hpp"
#include <string>

namespace nuby::net {

struct URL {
    std::string scheme{"http"};
    std::string host;
    int port{80};
    std::string path{"/"};
    std::string query;
    std::string fragment;

    static URL parse(const std::string& raw) {
        URL url;
        std::string s = core::StringUtils::trim(raw);
        if (s.empty()) return url;

        size_t scheme_pos = s.find("://");
        if (scheme_pos != std::string::npos) {
            url.scheme = core::StringUtils::to_lower(s.substr(0, scheme_pos));
            s = s.substr(scheme_pos + 3);
        }

        if (url.scheme == "https") {
            url.port = 443;
        } else {
            url.port = 80;
        }

        size_t slash_pos = s.find('/');
        std::string host_port = (slash_pos != std::string::npos) ? s.substr(0, slash_pos) : s;
        url.path = (slash_pos != std::string::npos) ? s.substr(slash_pos) : "/";

        size_t colon_pos = host_port.find(':');
        if (colon_pos != std::string::npos) {
            url.host = host_port.substr(0, colon_pos);
            try {
                url.port = std::stoi(host_port.substr(colon_pos + 1));
            } catch (...) {}
        } else {
            url.host = host_port;
        }

        size_t hash_pos = url.path.find('#');
        if (hash_pos != std::string::npos) {
            url.fragment = url.path.substr(hash_pos + 1);
            url.path = url.path.substr(0, hash_pos);
        }

        size_t q_pos = url.path.find('?');
        if (q_pos != std::string::npos) {
            url.query = url.path.substr(q_pos + 1);
            url.path = url.path.substr(0, q_pos);
        }

        if (url.path.empty()) url.path = "/";
        return url;
    }

    std::string to_string() const {
        std::string res = scheme + "://" + host;
        if ((scheme == "http" && port != 80) || (scheme == "https" && port != 443)) {
            res += ":" + std::to_string(port);
        }
        res += path;
        if (!query.empty()) res += "?" + query;
        if (!fragment.empty()) res += "#" + fragment;
        return res;
    }
};

} // namespace nuby::net
