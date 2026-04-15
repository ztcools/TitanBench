#include "protocol/http.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <string>
#include <utility>

namespace titanbench {
namespace http {
namespace {

constexpr char kCrLf[] = "\r\n";

std::string ToLowerAscii(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c >= 'A' && c <= 'Z') {
      out.push_back(static_cast<char>(c - 'A' + 'a'));
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string TrimAscii(std::string_view s) {
  std::size_t begin = 0;
  std::size_t end = s.size();
  while (begin < end &&
         std::isspace(static_cast<unsigned char>(s[begin])) != 0) {
    ++begin;
  }
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) {
    --end;
  }
  return std::string(s.substr(begin, end - begin));
}

bool HeaderExists(const std::vector<Header>& headers, std::string_view key) {
  const std::string lower_key = ToLowerAscii(key);
  for (const Header& h : headers) {
    if (ToLowerAscii(h.key) == lower_key) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::string BuildGetRequest(const Request& request) {
  std::string path = request.path.empty() ? "/" : request.path;
  if (path.front() != '/') {
    path.insert(path.begin(), '/');
  }

  std::string out;
  out.reserve(256 + request.host.size() + path.size() +
              request.headers.size() * 32);
  out.append("GET ");
  out.append(path);
  out.append(" HTTP/1.1");
  out.append(kCrLf);

  out.append("Host: ");
  out.append(request.host);
  out.append(kCrLf);

  bool has_connection = false;
  for (const Header& h : request.headers) {
    if (ToLowerAscii(h.key) == "connection") {
      has_connection = true;
    }
    out.append(h.key);
    out.append(": ");
    out.append(h.value);
    out.append(kCrLf);
  }

  if (!has_connection) {
    out.append("Connection: ");
    out.append(request.keep_alive ? "keep-alive" : "close");
    out.append(kCrLf);
  }
  if (!HeaderExists(request.headers, "accept")) {
    out.append("Accept: */*");
    out.append(kCrLf);
  }

  out.append(kCrLf);
  return out;
}

ResponseParser::ResponseParser() { Reset(); }

void ResponseParser::Reset() {
  state_ = State::kStatusLine;
  response_ = Response {};
  error_message_.clear();
  pending_.clear();
  body_.clear();
}

bool ResponseParser::Feed(const char* data, std::size_t len) {
  if (state_ == State::kError || state_ == State::kDone) {
    return state_ == State::kDone;
  }
  if (data == nullptr || len == 0) {
    return true;
  }

  pending_.append(data, len);
  while (true) {
    switch (state_) {
      case State::kStatusLine:
        if (!ParseStatusLine()) {
          return false;
        }
        if (state_ != State::kHeaders) {
          return state_ != State::kError;
        }
        break;
      case State::kHeaders:
        if (!ParseHeaders()) {
          return false;
        }
        if (state_ != State::kBody) {
          return state_ != State::kError;
        }
        break;
      case State::kBody:
        if (!ParseBody()) {
          return false;
        }
        return true;
      case State::kDone:
        return true;
      case State::kError:
        return false;
    }
  }
}

void ResponseParser::MarkEof() {
  if (state_ == State::kBody && !response_.has_content_length) {
    body_.append(pending_);
    pending_.clear();
    response_.content_length = body_.size();
    response_.message_complete = true;
    state_ = State::kDone;
  }
}

bool ResponseParser::ParseStatusLine() {
  const std::size_t pos = pending_.find("\r\n");
  if (pos == std::string::npos) {
    return true;
  }

  const std::string_view line(pending_.data(), pos);
  if (line.size() < 12 || line.substr(0, 5) != "HTTP/") {
    SetError("invalid status line");
    return false;
  }

  const std::size_t first_space = line.find(' ');
  if (first_space == std::string::npos || first_space + 4 > line.size()) {
    SetError("invalid status line format");
    return false;
  }

  const std::string_view code_text = line.substr(first_space + 1, 3);
  int code = 0;
  auto [ptr, ec] = std::from_chars(code_text.data(),
                                   code_text.data() + code_text.size(), code);
  if (ec != std::errc() || ptr != code_text.data() + code_text.size()) {
    SetError("invalid status code");
    return false;
  }

  response_.status_code = code;
  pending_.erase(0, pos + 2);
  state_ = State::kHeaders;
  return true;
}

bool ResponseParser::ParseHeaders() {
  const std::size_t pos = pending_.find("\r\n\r\n");
  if (pos == std::string::npos) {
    return true;
  }

  std::size_t line_begin = 0;
  while (line_begin < pos) {
    const std::size_t line_end = pending_.find("\r\n", line_begin);
    if (line_end == std::string::npos || line_end > pos) {
      SetError("invalid header line");
      return false;
    }

    const std::string_view line(pending_.data() + line_begin,
                                line_end - line_begin);
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) {
      SetError("malformed header");
      return false;
    }

    const std::string key = ToLowerAscii(TrimAscii(line.substr(0, colon)));
    const std::string value = ToLowerAscii(TrimAscii(line.substr(colon + 1)));

    if (key == "content-length") {
      std::size_t parsed_len = 0;
      auto [ptr, ec] = std::from_chars(value.data(),
                                       value.data() + value.size(), parsed_len);
      if (ec != std::errc() || ptr != value.data() + value.size()) {
        SetError("invalid content-length");
        return false;
      }
      response_.has_content_length = true;
      response_.content_length = parsed_len;
    } else if (key == "connection") {
      if (value == "close") {
        response_.keep_alive = false;
      } else if (value == "keep-alive") {
        response_.keep_alive = true;
      }
    }
    line_begin = line_end + 2;
  }

  response_.header_done = true;
  pending_.erase(0, pos + 4);
  state_ = State::kBody;
  return ParseBody();
}

bool ResponseParser::ParseBody() {
  if (!response_.has_content_length) {
    // 无长度时只能在连接 EOF 判定完成。
    body_.append(pending_);
    pending_.clear();
    return true;
  }

  const std::size_t need = response_.content_length;
  if (body_.size() < need) {
    const std::size_t take = std::min(need - body_.size(), pending_.size());
    body_.append(pending_.data(), take);
    pending_.erase(0, take);
  }

  if (body_.size() == need) {
    response_.message_complete = true;
    state_ = State::kDone;
    return true;
  }
  if (body_.size() > need) {
    SetError("body larger than content-length");
    return false;
  }
  return true;
}

void ResponseParser::SetError(std::string msg) {
  error_message_ = std::move(msg);
  state_ = State::kError;
}

TransactionTimer::TransactionTimer() : begin_(std::chrono::steady_clock::now()) {}

std::int64_t TransactionTimer::ElapsedNs() const {
  const auto now = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now - begin_)
      .count();
}

}  // namespace http
}  // namespace titanbench
