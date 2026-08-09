#pragma once

#include "url.hpp"
#include "../core/string_utils.hpp"
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <iostream>

namespace nuby::net {

struct HTTPResponse {
    int status_code{0};
    std::string status_message;
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    bool is_success() const { return status_code >= 200 && status_code < 300; }
    bool is_redirect() const { return status_code >= 300 && status_code < 400; }

    std::string get_header(const std::string& name) const {
        std::string lower = core::StringUtils::to_lower(name);
        for (const auto& [k, v] : headers) {
            if (core::StringUtils::to_lower(k) == lower) return v;
        }
        return "";
    }
};

class HTTPClient {
public:
    static HTTPResponse get(const URL& url, int max_redirects = 5) {
        HTTPResponse response;
        if (url.host.empty()) {
            response.status_code = 400;
            response.status_message = "Invalid URL";
            return response;
        }

        // Create TCP socket
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            response.status_code = 500;
            response.status_message = "Socket creation failed";
            return response;
        }

        // Set timeout 5s
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);

        // Resolve DNS
        struct hostent* server = gethostbyname(url.host.c_str());
        if (!server) {
            close(sock);
            response.status_code = 504;
            response.status_message = "DNS Resolution Failed";
            return response;
        }

        struct sockaddr_in serv_addr{};
        serv_addr.sin_family = AF_INET;
        std::memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
        serv_addr.sin_port = htons(url.port);

        // Connect
        if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
            close(sock);
            response.status_code = 502;
            response.status_message = "Connection Refused";
            return response;
        }

        // Build HTTP/1.1 Request
        std::ostringstream req;
        req << "GET " << url.path << (url.query.empty() ? "" : "?" + url.query) << " HTTP/1.1\r\n";
        req << "Host: " << url.host << "\r\n";
        req << "User-Agent: NubyEngine/1.0 (Next-Gen High Performance C++20 Browser Engine)\r\n";
        req << "Accept: text/html,text/css,image/*,*/*\r\n";
        req << "Connection: close\r\n\r\n";

        std::string req_str = req.str();
        send(sock, req_str.c_str(), req_str.length(), 0);

        // Read Response
        std::string raw_response;
        char buffer[4096];
        ssize_t bytes_read;
        while ((bytes_read = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
            buffer[bytes_read] = '\0';
            raw_response.append(buffer, bytes_read);
        }
        close(sock);

        // Parse Response Header & Body
        size_t header_end = raw_response.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            header_end = raw_response.find("\n\n");
            if (header_end == std::string::npos) {
                response.body = raw_response;
                response.status_code = 200;
                return response;
            }
        }

        std::string header_section = raw_response.substr(0, header_end);
        response.body = raw_response.substr(header_end + ((raw_response[header_end] == '\r') ? 4 : 2));

        // Parse Status Line
        std::istringstream hss(header_section);
        std::string status_line;
        if (std::getline(hss, status_line)) {
            auto parts = core::StringUtils::split_whitespace(status_line);
            if (parts.size() >= 2) {
                try {
                    response.status_code = std::stoi(parts[1]);
                } catch (...) {}
            }
            if (parts.size() >= 3) {
                response.status_message = parts[2];
            }
        }

        // Parse Headers
        std::string line;
        while (std::getline(hss, line)) {
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string k = core::StringUtils::trim(line.substr(0, colon));
                std::string v = core::StringUtils::trim(line.substr(colon + 1));
                response.headers[k] = v;
            }
        }

        // Follow redirects if needed
        if (response.is_redirect() && max_redirects > 0) {
            std::string location = response.get_header("Location");
            if (!location.empty()) {
                URL next_url = URL::parse(location);
                if (next_url.host.empty()) {
                    next_url.scheme = url.scheme;
                    next_url.host = url.host;
                    next_url.port = url.port;
                }
                return get(next_url, max_redirects - 1);
            }
        }

        return response;
    }
};

} // namespace nuby::net
