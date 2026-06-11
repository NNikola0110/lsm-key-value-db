// lsmkv - CLI entry point (Section 0).
// Commands exist and print helpful messages; storage ops report "not implemented yet".

#include "lsm/config.hpp"
#include "lsm/engine.hpp"
#include "lsm/errors.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

namespace fs = std::filesystem;

void print_usage(std::ostream& os) {
    os << "usage: lsmkv <command> [flags]\n"
          "\n"
          "commands:\n"
          "  init                       prepare data dir, print resolved config\n"
          "  put   --key K --value V    store a value (not implemented yet)\n"
          "  get   --key K              read a value (not implemented yet)\n"
          "  del   --key K              delete a key (not implemented yet)\n"
          "  stats                      print resolved config and engine status\n"
          "  close                      close the store\n"
          "\n"
          "global flags:\n"
          "  --config PATH              config file (default: config/default.json)\n";
}

// Parse "--flag value" and "--flag=value" tokens into a map.
// A flag with no following value (or followed by another flag) maps to "".
std::unordered_map<std::string, std::string>
parse_flags(const std::vector<std::string>& args, std::size_t start) {
    std::unordered_map<std::string, std::string> flags;
    for (std::size_t i = start; i < args.size(); ++i) {
        const std::string& tok = args[i];
        if (!tok.starts_with("--")) {
            continue;
        }
        std::string name = tok.substr(2);
        std::string value;
        if (auto eq = name.find('='); eq != std::string::npos) {
            value = name.substr(eq + 1);
            name = name.substr(0, eq);
        } else if (i + 1 < args.size() && !args[i + 1].starts_with("--")) {
            value = args[++i];
        }
        flags[name] = value;
    }
    return flags;
}

fs::path resolve_config_path(const std::unordered_map<std::string, std::string>& flags) {
    if (auto it = flags.find("config"); it != flags.end() && !it->second.empty()) {
        return fs::path(it->second);
    }
    return fs::path("config") / "default.json";
}

int run_mutation(const std::string& cmd,
                 const std::unordered_map<std::string, std::string>& flags,
                 const fs::path& config_path) {
    auto key_it = flags.find("key");
    if (key_it == flags.end() || key_it->second.empty()) {
        std::cerr << "error: --key is required for '" << cmd << "'\n";
        return 2;
    }
    const std::string& key = key_it->second;

    lsm::Config cfg = lsm::Config::load(config_path);
    lsm::Engine engine(cfg);

    try {
        if (cmd == "put") {
            auto val_it = flags.find("value");
            engine.put(key, val_it != flags.end() ? val_it->second : std::string_view{});
        } else if (cmd == "get") {
            (void)engine.get(key);
        } else { // del
            engine.remove(key);
        }
    } catch (const lsm::Error& e) {
        if (e.code() == lsm::ErrorCode::NotImplemented) {
            const char* verb = (cmd == "put") ? "Put" : (cmd == "get") ? "Get" : "Delete";
            std::cout << verb << '(' << key << ") — not implemented yet\n";
            return 0;
        }
        throw;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        print_usage(std::cerr);
        return 2;
    }

    const std::string cmd = args[0];
    const auto flags = parse_flags(args, 1);
    const fs::path config_path = resolve_config_path(flags);

    try {
        if (cmd == "init") {
            lsm::Config cfg = lsm::Config::load(config_path);
            std::error_code ec;
            fs::create_directories(cfg.data_dir, ec);
            if (ec) {
                throw lsm::Error(lsm::ErrorCode::IOFailure,
                                 "cannot create data dir '" + cfg.data_dir + "': " + ec.message());
            }
            std::cout << "config loaded ✓, data dir ready ✓, manifest placeholder ✓\n";
            return 0;
        }

        if (cmd == "put" || cmd == "get" || cmd == "del") {
            return run_mutation(cmd, flags, config_path);
        }

        if (cmd == "stats") {
            lsm::Config cfg = lsm::Config::load(config_path);
            cfg.print(std::cout);
            std::cout << "engine status: stub\n";
            return 0;
        }

        if (cmd == "close") {
            std::cout << "closed (stub)\n";
            return 0;
        }

        if (cmd == "-h" || cmd == "--help" || cmd == "help") {
            print_usage(std::cout);
            return 0;
        }

        std::cerr << "unknown command: " << cmd << "\n\n";
        print_usage(std::cerr);
        return 2;

    } catch (const lsm::Error& e) {
        std::cerr << '[' << lsm::to_string(e.code()) << "] " << e.what() << '\n';
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
