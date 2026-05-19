#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string_view>
#include <iostream>
#include <string>
namespace asio = boost::asio;
using tcp = boost::asio::ip::tcp;
namespace websocket = boost::beast::websocket;

static std::string trim(std::string value) {
    const std::size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const std::size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

static std::string json_field_value(const std::string& json, std::string_view key) {
    const std::string token = std::string("\"") + std::string(key) + "\":";
    const std::size_t key_pos = json.find(token);
    std::size_t start = key_pos + token.size();
    while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start]))) {
        ++start;
    }

    if (start >= json.size()) {
        return {};
    }

    if (json[start] == '"') {
        const std::size_t second_quote = json.find('"', start + 1);
        if (second_quote == std::string::npos) {
            return {};
        }
        return json.substr(start + 1, second_quote - start - 1);
    }

    std::size_t end = start;
    while (end < json.size()) {
        const char ch = json[end];
        if (ch == ',' || ch == '}' || ch == ']' || std::isspace(static_cast<unsigned char>(ch))) {
            break;
        }
        ++end;
    }

    if (end == start) {
        return {};
    }

    return json.substr(start, end - start);
}

static double to_double(const std::string& text) { return std::stod(text); }

static std::string to_lower(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return v;
}

struct Options {
    std::string symbol = "BTCUSDT";
    int count = 0;
    bool normalize = false;
};

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--symbol" && i + 1 < argc) o.symbol = argv[++i];
        else if (a == "--count" && i + 1 < argc) o.count = std::stoi(argv[++i]);
        else if (a == "--normalize" || a == "--numeric") o.normalize = true;
        else if (a == "--help" || a == "-h") {
            std::cout << "Usage: binance_ws [--symbol SYMBOL] [--count N] [--normalize]\n";
            std::exit(0);
        }
    }
    return o;
}

int main(int argc, char** argv) {
    try {
        const Options opt = parse_args(argc, argv);
        std::cout.setf(std::ios::unitbuf);

        boost::asio::io_context ioc;
        boost::asio::ssl::context ctx{boost::asio::ssl::context::tlsv12_client};
        ctx.set_default_verify_paths();

        tcp::resolver resolver{ioc};
        websocket::stream<boost::asio::ssl::stream<tcp::socket>> ws{ioc, ctx};

        const std::string host = "stream.binance.com";
        const std::string port = "9443";
        const std::string path = "/ws/" + to_lower(opt.symbol) + "@bookTicker";

        auto const results = resolver.resolve(host, port);
        boost::asio::connect(boost::beast::get_lowest_layer(ws), results.begin(), results.end());

        // perform SSL handshake
        ws.next_layer().handshake(boost::asio::ssl::stream_base::client);

        // set a reasonable timeout
        ws.set_option(websocket::stream_base::timeout::suggested(boost::beast::role_type::client));

        // websocket handshake
        ws.handshake(host, path);
        std::cerr << "Connected to " << host << path << "\n";

        int received = 0;
        for (;;) {
            boost::beast::flat_buffer buffer;
            ws.read(buffer);
            const std::string msg = boost::beast::buffers_to_string(buffer.data());

            const std::string sym = json_field_value(msg, "s");
            const std::string btext = json_field_value(msg, "b");
            const std::string atext = json_field_value(msg, "a");
            const std::string Btext = json_field_value(msg, "B");
            const std::string Atext = json_field_value(msg, "A");

            if (sym.empty() || btext.empty() || atext.empty()) {
                continue;
            }

            const double bid = to_double(btext);
            const double ask = to_double(atext);
            double bid_size = 0.0;
            double ask_size = 0.0;
            if (!Btext.empty()) {
                bid_size = to_double(Btext);
            }
            if (!Atext.empty()) {
                ask_size = to_double(Atext);
            }

            if (opt.normalize) {
                const double vol = bid_size + ask_size;
                std::cout << bid << ' ' << ask << ' ' << vol << '\n';
            } else {
                std::cout << '[' << sym << "] bid=" << bid << " ask=" << ask << " bid_size=" << bid_size << " ask_size=" << ask_size << '\n';
            }

            ++received;
            if (opt.count > 0 && received >= opt.count) {
                break;
            }
        }

        boost::system::error_code ec;
        ws.close(websocket::close_code::normal, ec);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
