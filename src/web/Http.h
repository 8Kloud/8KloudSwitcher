/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

// The wire-level pieces of the web GUI server, kept free of sockets so they
// are unit-testable: HTTP/1.1 request parsing and response framing, and the
// RFC 6455 WebSocket handshake and frame codec (SHA-1 and base64 included --
// the only cryptography a WebSocket server needs, and not worth a
// dependency).
namespace kloud::web {

enum class Parse { Incomplete, Ok, Bad };

struct HttpRequest {
    std::string method;
    std::string target;  // as sent, e.g. "/api/fs?path=%2Fhome"
    std::string path;    // percent-decoded path part
    std::string query;   // raw query string (no '?')
    std::map<std::string, std::string> headers;  // lower-case names
    std::string body;

    std::string header(std::string_view name) const;
    bool wantsUpgradeToWebSocket() const;
};

// Parses one request (head + Content-Length body) from the front of `in`.
// `consumed` is set on Ok. Heads over 16 KiB or bodies over `maxBody` are
// Bad.
Parse parseHttpRequest(std::string_view in, HttpRequest& out, size_t& consumed,
                       size_t maxBody = 1 << 20);

std::string httpResponse(int status, std::string_view contentType,
                         std::string_view body,
                         std::string_view extraHeaders = {});
const char* httpStatusText(int status);

// Decodes %XX and leaves everything else (including '+') alone.
std::string percentDecode(std::string_view s);
// Value of `key` in a query string, decoded; empty when absent.
std::string queryParam(std::string_view query, std::string_view key);

// Content type by file extension (text/html, application/javascript, ...).
std::string_view contentTypeFor(std::string_view path);

void sha1(const uint8_t* data, size_t len, uint8_t digest[20]);
std::string base64(const uint8_t* data, size_t len);
// Sec-WebSocket-Accept for a Sec-WebSocket-Key.
std::string websocketAccept(std::string_view key);
std::string websocketHandshakeResponse(std::string_view key);

struct WsFrame {
    uint8_t opcode = 0;  // 0 continuation, 1 text, 2 binary, 8 close, 9 ping, 10 pong
    bool fin = true;
    std::string payload;  // unmasked
};

// Client frames must be masked (Bad otherwise). Payloads over `maxPayload`
// are Bad.
Parse parseWsFrame(std::string_view in, WsFrame& out, size_t& consumed,
                   size_t maxPayload = 1 << 20);
// Server -> client frame (never masked).
std::string wsFrame(uint8_t opcode, std::string_view payload);
inline std::string wsText(std::string_view s) { return wsFrame(1, s); }
inline std::string wsBinary(std::string_view s) { return wsFrame(2, s); }
inline std::string wsClose(uint16_t code = 1000) {
    const char body[2] = {char(code >> 8), char(code & 0xff)};
    return wsFrame(8, std::string_view(body, 2));
}

}  // namespace kloud::web
