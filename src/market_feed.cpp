#include <arpa/inet.h>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

volatile std::sig_atomic_t g_running = 1;

void on_sigint(int) {
    g_running = 0;
}

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string trim(std::string value) {
    const std::size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::vector<std::string> split_csv(const std::string& value) {
    std::vector<std::string> items;
    std::stringstream stream(value);
    std::string item;
    while (std::getline(stream, item, ',')) {
        item = trim(item);
        if (!item.empty()) {
            items.push_back(item);
        }
    }
    return items;
}

std::string get_env(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

std::string base64_encode(const unsigned char* data, std::size_t size) {
    std::string encoded(((size + 2) / 3) * 4, '\0');
    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()), data, static_cast<int>(size));
    encoded.resize(static_cast<std::size_t>(written));
    return encoded;
}

std::string websocket_accept_key(const std::string& client_key) {
    const std::string combined = client_key + std::string(kWebSocketGuid);
    std::array<unsigned char, SHA_DIGEST_LENGTH> digest{};
    SHA1(reinterpret_cast<const unsigned char*>(combined.data()), combined.size(), digest.data());
    return base64_encode(digest.data(), digest.size());
}

std::string random_key() {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("failed to generate websocket key");
    }
    return base64_encode(bytes.data(), bytes.size());
}

std::string json_string_field(const std::string& json, std::string_view key) {
    const std::string token = std::string("\"") + std::string(key) + "\":";
    const std::size_t key_pos = json.find(token);
    if (key_pos == std::string::npos) {
        return {};
    }
    const std::size_t first_quote = json.find('"', key_pos + token.size());
    if (first_quote == std::string::npos) {
        return {};
    }
    const std::size_t second_quote = json.find('"', first_quote + 1);
    if (second_quote == std::string::npos) {
        return {};
    }
    return json.substr(first_quote + 1, second_quote - first_quote - 1);
}

std::string json_number_field(const std::string& json, std::string_view key) {
    const std::string token = std::string("\"") + std::string(key) + "\":";
    const std::size_t key_pos = json.find(token);
    if (key_pos == std::string::npos) {
        return {};
    }
    std::size_t start = key_pos + token.size();
    while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start]))) {
        ++start;
    }
    std::size_t end = start;
    while (end < json.size()) {
        const char ch = json[end];
        if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '+' || ch == 'e' || ch == 'E') {
            ++end;
            continue;
        }
        break;
    }
    if (end == start) {
        return {};
    }
    return json.substr(start, end - start);
}

double to_double(const std::string& text) {
    return std::stod(text);
}

struct ParsedArguments {
    std::string exchange = "binance";
    std::string symbol = "BTCUSDT";
    std::vector<std::string> symbols = {"AAPL"};
    std::string api_key;
    std::string secret_key;
    std::string alpaca_feed = "iex";
    int count = 0;
    bool normalize = false;
    bool test_mode = false;
};

struct Endpoint {
    std::string host;
    std::string port;
    std::string path;
};

class TlsWebSocket {
public:
    ~TlsWebSocket() {
        close();
    }

    bool connect(const Endpoint& endpoint) {
        endpoint_ = endpoint;

        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) {
            print_ssl_error("SSL_CTX_new");
            return false;
        }

        fd_ = open_socket(endpoint.host, endpoint.port);
        if (fd_ < 0) {
            return false;
        }

        ssl_ = SSL_new(ctx_);
        if (!ssl_) {
            print_ssl_error("SSL_new");
            return false;
        }

        SSL_set_tlsext_host_name(ssl_, endpoint.host.c_str());
        SSL_set_fd(ssl_, fd_);
        if (SSL_connect(ssl_) != 1) {
            print_ssl_error("SSL_connect");
            return false;
        }

        return handshake();
    }

    bool send_text(const std::string& text) {
        return send_frame(0x1, text);
    }

    bool read_text(std::string& text) {
        while (g_running) {
            unsigned char header[2]{};
            if (!read_exact(reinterpret_cast<char*>(header), sizeof(header))) {
                return false;
            }

            const unsigned char opcode = header[0] & 0x0F;
            const bool masked = (header[1] & 0x80) != 0;
            std::uint64_t length = header[1] & 0x7F;

            if (length == 126) {
                unsigned char extended[2]{};
                if (!read_exact(reinterpret_cast<char*>(extended), sizeof(extended))) {
                    return false;
                }
                length = (static_cast<std::uint64_t>(extended[0]) << 8) | extended[1];
            } else if (length == 127) {
                unsigned char extended[8]{};
                if (!read_exact(reinterpret_cast<char*>(extended), sizeof(extended))) {
                    return false;
                }
                length = 0;
                for (unsigned char byte : extended) {
                    length = (length << 8) | byte;
                }
            }

            std::array<unsigned char, 4> mask{};
            if (masked && !read_exact(reinterpret_cast<char*>(mask.data()), mask.size())) {
                return false;
            }

            std::string payload(length, '\0');
            if (!read_exact(payload.data(), static_cast<std::size_t>(length))) {
                return false;
            }

            if (masked) {
                for (std::size_t i = 0; i < payload.size(); ++i) {
                    payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i % 4]);
                }
            }

            if (opcode == 0x1) {
                text = std::move(payload);
                return true;
            }
            if (opcode == 0x8) {
                return false;
            }
            if (opcode == 0x9) {
                if (!send_frame(0xA, payload)) {
                    return false;
                }
            }
        }
        return false;
    }

private:
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    int fd_ = -1;
    Endpoint endpoint_;
    std::string buffer_;

    static void print_ssl_error(const char* what) {
        std::cerr << what << " failed: " << ERR_error_string(ERR_get_error(), nullptr) << '\n';
    }

    static int open_socket(const std::string& host, const std::string& port) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo* result = nullptr;
        const int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &result);
        if (rc != 0) {
            std::cerr << "getaddrinfo(" << host << ", " << port << ") failed: " << gai_strerror(rc) << '\n';
            return -1;
        }

        int fd = -1;
        for (addrinfo* entry = result; entry != nullptr; entry = entry->ai_next) {
            fd = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
            if (fd < 0) {
                continue;
            }
            if (::connect(fd, entry->ai_addr, entry->ai_addrlen) == 0) {
                break;
            }
            ::close(fd);
            fd = -1;
        }

        freeaddrinfo(result);
        return fd;
    }

    bool handshake() {
        const std::string client_key = random_key();
        const std::string expected_accept = websocket_accept_key(client_key);
        std::ostringstream request;
        request << "GET " << endpoint_.path << " HTTP/1.1\r\n"
                << "Host: " << endpoint_.host << ':' << endpoint_.port << "\r\n"
                << "Upgrade: websocket\r\n"
                << "Connection: Upgrade\r\n"
                << "Sec-WebSocket-Key: " << client_key << "\r\n"
                << "Sec-WebSocket-Version: 13\r\n"
                << "User-Agent: FluxionMarketFeed/1.0\r\n\r\n";

        const std::string request_text = request.str();
        if (!write_all(request_text.data(), request_text.size())) {
            return false;
        }

        std::string response;
        char chunk[4096];
        while (response.find("\r\n\r\n") == std::string::npos) {
            const int got = SSL_read(ssl_, chunk, sizeof(chunk));
            if (got <= 0) {
                print_ssl_error("SSL_read");
                return false;
            }
            response.append(chunk, chunk + got);
        }

        const std::size_t header_end = response.find("\r\n\r\n");
        const std::string headers = response.substr(0, header_end);
        buffer_ = response.substr(header_end + 4);

        if (headers.find(" 101 ") == std::string::npos) {
            std::cerr << "websocket handshake failed:\n" << headers << '\n';
            return false;
        }

        const std::string accept_token = "Sec-WebSocket-Accept: ";
        const std::size_t accept_pos = headers.find(accept_token);
        if (accept_pos == std::string::npos) {
            std::cerr << "missing Sec-WebSocket-Accept header\n";
            return false;
        }

        const std::size_t line_end = headers.find("\r\n", accept_pos);
        const std::string accept_value = trim(headers.substr(accept_pos + accept_token.size(), line_end - (accept_pos + accept_token.size())));
        if (accept_value != expected_accept) {
            std::cerr << "invalid Sec-WebSocket-Accept value\n";
            return false;
        }

        return true;
    }

    bool write_all(const char* data, std::size_t size) {
        std::size_t written = 0;
        while (written < size) {
            const int rc = SSL_write(ssl_, data + written, static_cast<int>(size - written));
            if (rc <= 0) {
                print_ssl_error("SSL_write");
                return false;
            }
            written += static_cast<std::size_t>(rc);
        }
        return true;
    }

    bool send_frame(unsigned char opcode, const std::string& payload) {
        std::vector<unsigned char> frame;
        frame.push_back(static_cast<unsigned char>(0x80 | (opcode & 0x0F)));

        std::array<unsigned char, 4> mask{};
        if (RAND_bytes(mask.data(), static_cast<int>(mask.size())) != 1) {
            print_ssl_error("RAND_bytes");
            return false;
        }

        const std::uint64_t size = payload.size();
        if (size < 126) {
            frame.push_back(static_cast<unsigned char>(0x80 | static_cast<unsigned char>(size)));
        } else if (size <= 0xFFFF) {
            frame.push_back(0x80 | 126);
            frame.push_back(static_cast<unsigned char>((size >> 8) & 0xFF));
            frame.push_back(static_cast<unsigned char>(size & 0xFF));
        } else {
            frame.push_back(0x80 | 127);
            for (int shift = 56; shift >= 0; shift -= 8) {
                frame.push_back(static_cast<unsigned char>((size >> shift) & 0xFF));
            }
        }

        frame.insert(frame.end(), mask.begin(), mask.end());
        const std::size_t offset = frame.size();
        frame.resize(offset + payload.size());
        for (std::size_t i = 0; i < payload.size(); ++i) {
            frame[offset + i] = static_cast<unsigned char>(payload[i]) ^ mask[i % 4];
        }

        return write_all(reinterpret_cast<const char*>(frame.data()), frame.size());
    }

    bool read_exact(char* out, std::size_t size) {
        std::size_t copied = 0;

        if (!buffer_.empty()) {
            const std::size_t take = std::min(size, buffer_.size());
            std::memcpy(out, buffer_.data(), take);
            buffer_.erase(0, take);
            copied += take;
        }

        while (copied < size) {
            const int rc = SSL_read(ssl_, out + copied, static_cast<int>(size - copied));
            if (rc <= 0) {
                print_ssl_error("SSL_read");
                return false;
            }
            copied += static_cast<std::size_t>(rc);
        }

        return true;
    }

    void close() {
        if (ssl_) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        if (ctx_) {
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
        }
    }
};

ParsedArguments parse_arguments(int argc, char** argv) {
    ParsedArguments options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];

        const auto require_value = [&](const char* name) -> std::string {
            if (index + 1 >= argc) {
                throw std::runtime_error(std::string(name) + " requires a value");
            }
            return argv[++index];
        };

        if (argument == "--exchange") {
            options.exchange = to_lower(require_value("--exchange"));
        } else if (argument == "--symbol") {
            options.symbol = require_value("--symbol");
        } else if (argument == "--symbols") {
            options.symbols = split_csv(require_value("--symbols"));
        } else if (argument == "--api-key") {
            options.api_key = require_value("--api-key");
        } else if (argument == "--secret-key") {
            options.secret_key = require_value("--secret-key");
        } else if (argument == "--alpaca-feed") {
            options.alpaca_feed = to_lower(require_value("--alpaca-feed"));
        } else if (argument == "--count") {
            options.count = std::stoi(require_value("--count"));
        } else if (argument == "--normalize" || argument == "--numeric") {
            options.normalize = true;
        } else if (argument == "--test") {
            options.test_mode = true;
        } else if (argument == "--help" || argument == "-h") {
            std::cout << "Usage:\n"
                      << "  ./build/market_feed --exchange binance --symbol BTCUSDT\n"
                      << "  ./build/market_feed --exchange alpaca --symbol AAPL --api-key KEY --secret-key SECRET\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + argument);
        }
    }

    if (options.api_key.empty()) {
        options.api_key = get_env("ALPACA_API_KEY");
    }
    if (options.secret_key.empty()) {
        options.secret_key = get_env("ALPACA_SECRET_KEY");
    }
    return options;
}

Endpoint endpoint_for(const ParsedArguments& options) {
    if (options.exchange == "binance") {
        const std::string stream = to_lower(options.symbol) + "@bookTicker";
        return {"stream.binance.com", "9443", "/ws/" + stream};
    }
    if (options.exchange == "alpaca") {
        return {"stream.data.alpaca.markets", "443", "/v2/" + options.alpaca_feed};
    }
    throw std::runtime_error("unsupported exchange: " + options.exchange);
}

void print_quote(const std::string& exchange,
                 const std::string& symbol,
                 double bid,
                 double ask,
                 double bid_size,
                 double ask_size) {
    std::cout << '[' << exchange << "] " << symbol
              << " bid=" << std::fixed << std::setprecision(8) << bid
              << " ask=" << std::fixed << std::setprecision(8) << ask
              << " bid_size=" << std::fixed << std::setprecision(4) << bid_size
              << " ask_size=" << std::fixed << std::setprecision(4) << ask_size
              << '\n';
}

void print_numeric(double bid, double ask, double volume) {
    std::cout << std::fixed << std::setprecision(8) << bid << ' ' << std::fixed << std::setprecision(8) << ask << ' ' << std::fixed << std::setprecision(4) << volume << '\n';
}

bool run_binance(TlsWebSocket& socket, const ParsedArguments& options) {
    int emitted = 0;
    std::string message;
    while (g_running && socket.read_text(message)) {
        const std::string symbol = json_string_field(message, "s");
        const std::string bid_text = json_number_field(message, "b");
        const std::string ask_text = json_number_field(message, "a");
        const std::string bid_size_text = json_number_field(message, "B");
        const std::string ask_size_text = json_number_field(message, "A");
        if (symbol.empty() || bid_text.empty() || ask_text.empty() || bid_size_text.empty() || ask_size_text.empty()) {
            continue;
        }

        if (options.normalize) {
            const double bid = to_double(bid_text);
            const double ask = to_double(ask_text);
            const double volume = to_double(bid_size_text) + to_double(ask_size_text);
            print_numeric(bid, ask, volume);
        } else {
            print_quote("binance",
                        symbol,
                        to_double(bid_text),
                        to_double(ask_text),
                        to_double(bid_size_text),
                        to_double(ask_size_text));
        }

        ++emitted;
        if (options.count > 0 && emitted >= options.count) {
            return true;
        }
    }
    return emitted > 0;
}

bool run_alpaca(TlsWebSocket& socket, const ParsedArguments& options) {
    if (options.api_key.empty() || options.secret_key.empty()) {
        std::cerr << "Alpaca requires --api-key and --secret-key or ALPACA_API_KEY / ALPACA_SECRET_KEY\n";
        return false;
    }

    if (!socket.send_text(std::string("{\"action\":\"auth\",\"key\":\"") + options.api_key + "\",\"secret\":\"" + options.secret_key + "\"}")) {
        return false;
    }

    std::ostringstream subscribe;
    subscribe << "{\"action\":\"subscribe\",\"quotes\":[";
    for (std::size_t i = 0; i < options.symbols.size(); ++i) {
        if (i > 0) {
            subscribe << ',';
        }
        subscribe << '"' << options.symbols[i] << '"';
    }
    subscribe << "]}";
    if (!socket.send_text(subscribe.str())) {
        return false;
    }

    int emitted = 0;
    std::string payload;
    while (g_running && socket.read_text(payload)) {
        std::size_t cursor = 0;
        while (cursor < payload.size()) {
            const std::size_t open = payload.find('{', cursor);
            if (open == std::string::npos) {
                break;
            }

            int depth = 0;
            std::size_t close = std::string::npos;
            for (std::size_t i = open; i < payload.size(); ++i) {
                if (payload[i] == '{') {
                    ++depth;
                } else if (payload[i] == '}') {
                    --depth;
                    if (depth == 0) {
                        close = i;
                        break;
                    }
                }
            }
            if (close == std::string::npos) {
                break;
            }

            const std::string message = payload.substr(open, close - open + 1);
            cursor = close + 1;

            const std::string symbol = json_string_field(message, "S");
            const std::string bid_text = json_number_field(message, "bp");
            const std::string ask_text = json_number_field(message, "ap");
            const std::string bid_size_text = json_number_field(message, "bs");
            const std::string ask_size_text = json_number_field(message, "as");
            if (symbol.empty() || bid_text.empty() || ask_text.empty() || bid_size_text.empty() || ask_size_text.empty()) {
                continue;
            }

            if (options.normalize) {
                const double bid = to_double(bid_text);
                const double ask = to_double(ask_text);
                const double volume = to_double(bid_size_text) + to_double(ask_size_text);
                print_numeric(bid, ask, volume);
            } else {
                print_quote("alpaca",
                            symbol,
                            to_double(bid_text),
                            to_double(ask_text),
                            to_double(bid_size_text),
                            to_double(ask_size_text));
            }

            ++emitted;
            if (options.count > 0 && emitted >= options.count) {
                return true;
            }
        }
    }

    return emitted > 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, on_sigint);

    try {
        const ParsedArguments options = parse_arguments(argc, argv);
        if (options.test_mode) {
            // Emit a few sample numeric rows for testing pipelines
            for (int i = 0; i < (options.count > 0 ? options.count : 3); ++i) {
                // sample: bid ask volume
                print_numeric(50000.0 + i, 50000.5 + i, 0.1 + i);
            }
            return 0;
        }

        const Endpoint endpoint = endpoint_for(options);

        TlsWebSocket socket;
        if (!socket.connect(endpoint)) {
            std::cerr << "market_feed: connect failed\n";
            return 1;
        }
        std::cerr << "market_feed: connected to " << endpoint.host << ':' << endpoint.port << "\n";

        bool ok = false;
        if (options.exchange == "binance") {
            ok = run_binance(socket, options);
        } else if (options.exchange == "alpaca") {
            ok = run_alpaca(socket, options);
        } else {
            std::cerr << "unsupported exchange: " << options.exchange << '\n';
            return 1;
        }

        return ok ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "market_feed: " << error.what() << '\n';
        return 1;
    }
}