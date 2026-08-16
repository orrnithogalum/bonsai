/* CONFIG
Explanation:
- This class handles loading Bonsai's configuration from ~/.config/bonsai/bonsai.conf
- It provides default values if the file doesn't exist
- Configuration values include sidebar icons, chart parameters, colors, and UI sizing
- Singleton pattern ensures only one Config object exists at runtime
- The parser reads key=value lines, trims whitespace, and converts to the appropriate type
- Complex values (like CHART_COLORS) are parsed from array blocks in the config
- All file I/O, directory creation, and parsing errors are safely handled
*/

#pragma once

#include "gen/defaults.hpp"

#include <ftxui/component/component.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <pwd.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

class Config {
public:
    std::vector<std::array<int, 3>> CHART_COLORS;

    std::string SIDEBAR_SELECTED_FOLDER_ICON;
    std::string SIDEBAR_SELECTED_FILE_ICON;
    std::string SIDEBAR_FOLDER_ICON;
    std::string SIDEBAR_FILE_ICON;
    std::string SIDEBAR_BACK_ICON;

    std::array<int, 3> SIDEBAR_GROWTH_COLOR = {255, 175, 95};
    std::array<int, 3> SIDEBAR_SHRINK_COLOR = {95, 125, 255};

    int CHART_MAX_GENERATIONS = 4;
    int SIDEBAR_WIDTH = 40;

    double SIDEBAR_GROWTH_THRESHOLD_PERCENTAGE = 0.2;
    double CHART_MAX_SIZE_THRESHOLD_PERCENTAGE = 2;
    double CHART_DIM_FACTOR = 0.15;

    bool ENABLE_FOLDER_SIZE_PERCENTAGES = true;

    /* Lazy initialization using a lambda:
    - Ensures config is loaded once at first access
    - If the config file does not exist, create it with defaults
    - Parse the file, or throw if invalid
    */
    static const Config& get() {
        static Config instance = []() -> Config {
            if (!ensureConfigFile()) throw std::runtime_error("Cannot create config file");
            std::string path = getUserConfigPath();

            auto config = parseConfigFile(path);
            if (!config) throw std::runtime_error("Invalid config file");
            return *config;
        }();
        return instance;
    }

    /* Save the config file:
    - If user resizes the sidebar, then it's saved here
    - That is the only thing that can change programmatically, for now
    - All other keys are copied verbatim from the user's existing file so
      manual edits (comments, formatting, etc.) survive the rewrite
    */
    bool writeToFile() const {
        std::string path = getUserConfigPath();
        if (path.empty())
            return false;

        std::ifstream in(path);
        if (!in.is_open())
            return false;

        std::string userConfig(
            (std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>()
        );
        in.close();

        std::string config(
            reinterpret_cast<const char*>(bonsai_default),
            bonsai_default_len
        );

        // Every field's key lives in fieldTable() exactly once; passthrough
        // fields get copied verbatim from the user's existing file.
        for (const auto& field : fieldTable()) {
            if (field.passthrough) {
                copyValue(config, userConfig, field.key);
            }
        }

        // SIDEBAR_WIDTH is the one field this class can change at runtime,
        // so we write our in-memory value instead of copying from disk.
        copyValue(config, "SIDEBAR_WIDTH = " + std::to_string(SIDEBAR_WIDTH), "SIDEBAR_WIDTH");

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
            return false;

        out.write(config.data(), static_cast<std::streamsize>(config.size()));
        return out.good();
    }

    static ftxui::Color growthColor() {
        const auto& config = Config::get();
        return ftxui::Color::RGB(
            config.SIDEBAR_GROWTH_COLOR[0],
            config.SIDEBAR_GROWTH_COLOR[1],
            config.SIDEBAR_GROWTH_COLOR[2]
        );
    }

    static ftxui::Color shrinkColor() {
        const auto& config = Config::get();
        return ftxui::Color::RGB(
            config.SIDEBAR_SHRINK_COLOR[0],
            config.SIDEBAR_SHRINK_COLOR[1],
            config.SIDEBAR_SHRINK_COLOR[2]
        );
    }

private:
    // Returns path to ~/.config/bonsai/bonsai.conf or empty string if HOME not set
    static std::string getUserConfigPath() {
        if (const auto* pw = getpwuid(getuid()); pw && pw->pw_dir) {
            return (fs::path(pw->pw_dir) / ".config/bonsai/bonsai.conf").string();
        }
        return ".config/bonsai/bonsai.conf";
    }

    // Writes the default configuration binary to the given path
    static bool writeDefaultConfig(const std::string& path) {
        std::ofstream out(path, std::ios::binary);
        if (!out.is_open()) return false;

        out.write(reinterpret_cast<const char*>(bonsai_default), bonsai_default_len);
        return true;
    }

    /* Ensures the config file exists:
    - Creates parent directories if needed
    - Writes default config if file doesn't exist
    - Returns true if file is ready
    */
    static bool ensureConfigFile() {
        std::string config_path = getUserConfigPath();
        fs::path config_dir = fs::path(config_path).parent_path();

        if (!fs::exists(config_dir)) fs::create_directories(config_dir);
        if (!fs::exists(config_path)) return writeDefaultConfig(config_path);

        return true;
    }

    static void trim(std::string& s) {
        size_t start = s.find_first_not_of(" \t");
        if (start == std::string::npos) { s.clear(); return; }
        size_t end = s.find_last_not_of(" \t");
        s = s.substr(start, end - start + 1);
    }

    /* Finds value in config file for a given key
    - returns the starting index of the value string, and its length
    - for example, findValueSpan("PORT=8080", "PORT") would return <5, 4>
    */
    static std::pair<size_t, size_t> findValueSpan(const std::string& source, const std::string& key) {
        size_t key_pos = source.find(key);
        if (key_pos == std::string::npos) return {std::string::npos, 0};

        size_t eq = source.find('=', key_pos);
        if (eq == std::string::npos) return {std::string::npos, 0};

        size_t value_start = eq + 1;
        while (value_start < source.size() &&
               (source[value_start] == ' ' || source[value_start] == '\t')) {
            ++value_start;
        }

        size_t value_end = value_start;
        while (value_end < source.size() &&
               source[value_end] != '\n' &&
               source[value_end] != '\r' &&
               source[value_end] != '#') {
            ++value_end;
        }

        while (value_end > value_start &&
               (source[value_end - 1] == ' ' || source[value_end - 1] == '\t')) {
            --value_end;
        }

        return {value_start, value_end - value_start};
    }

    /* Replaces the value for a given key in the config string with the value for that key from the source string
    - for example, copyValue("PORT=8080", "PORT=3000", "PORT") would change "PORT=8080" to "PORT=3000"
    */
    static bool copyValue(std::string& config, const std::string& source, const std::string& key) {
        auto [source_start, source_len] = findValueSpan(source, key);
        if (source_start == std::string::npos) return false;
        std::string value = source.substr(source_start, source_len);

        auto [config_start, config_len] = findValueSpan(config, key);
        if (config_start == std::string::npos) return false;

        config.replace(config_start, config_len, value);
        return true;
    }

    static std::string unquote(const std::string& value) {
        if (value.size() < 2) return value;
        return value.substr(1, value.size() - 2);
    }

    static std::array<int, 3> parseColor(const std::string& value) {
        std::stringstream ss(value);
        std::array<int, 3> color{};
        char c;
        ss >> c >> color[0] >> c >> color[1] >> c >> color[2];
        return color;
    }

    /* Declarative field table
    - Source for every "simple" config field (as in anything that is one key = one value)
    - Previously this lived as separate hand-written lists that had to be kept in sync by hand.
      CHART_COLORS is the one field that can't fit in this table, since parsing it consumes multiple lines from the stream;
      It's still handled as a special case in parseConfigFile().
    */
    static int parseValue(int*, const std::string& v) {
        return std::stoi(v);
    }

    static double parseValue(double*, const std::string& v) {
        return std::stod(v);
    }

    static bool parseValue(bool*, const std::string& v) {
        return v == "true";
    }

    static std::string parseValue(std::string*, const std::string& v) {
        return unquote(v);
    }

    static std::array<int, 3> parseValue(std::array<int, 3>*, const std::string& v) {
        return parseColor(v);
    }

    using Setter = std::function<void(Config&, const std::string&)>;

    struct FieldSpec {
        Setter setter;

        const char* key;
        bool passthrough;
    };

    /* Builds a Setter for `member` with zero repetition of its type.
    - T is deduced from the member pointer.
    - parseValue() is picked by overload resolution on that same T.
    */
    template <typename T> static Setter makeSetter(T Config::* member) {
        return [member](Config& c, const std::string& v) {
            c.*member = parseValue(static_cast<T*>(nullptr), v);
        };
    }

    static const std::vector<FieldSpec>& fieldTable() {
        static const std::vector<FieldSpec> table = {
            {"SIDEBAR_WIDTH",                       makeSetter(&Config::SIDEBAR_WIDTH),                       false},
            {"SIDEBAR_SELECTED_FOLDER_ICON",        makeSetter(&Config::SIDEBAR_SELECTED_FOLDER_ICON),        true},
            {"SIDEBAR_SELECTED_FILE_ICON",          makeSetter(&Config::SIDEBAR_SELECTED_FILE_ICON),          true},
            {"SIDEBAR_FOLDER_ICON",                 makeSetter(&Config::SIDEBAR_FOLDER_ICON),                 true},
            {"SIDEBAR_FILE_ICON",                   makeSetter(&Config::SIDEBAR_FILE_ICON),                   true},
            {"SIDEBAR_BACK_ICON",                   makeSetter(&Config::SIDEBAR_BACK_ICON),                   true},
            {"SIDEBAR_GROWTH_COLOR",                makeSetter(&Config::SIDEBAR_GROWTH_COLOR),                true},
            {"SIDEBAR_SHRINK_COLOR",                makeSetter(&Config::SIDEBAR_SHRINK_COLOR),                true},
            {"CHART_MAX_SIZE_THRESHOLD_PERCENTAGE", makeSetter(&Config::CHART_MAX_SIZE_THRESHOLD_PERCENTAGE), true},
            {"CHART_MAX_GENERATIONS",               makeSetter(&Config::CHART_MAX_GENERATIONS),               true},
            {"CHART_DIM_FACTOR",                    makeSetter(&Config::CHART_DIM_FACTOR),                    true},
            {"ENABLE_FOLDER_SIZE_PERCENTAGES",      makeSetter(&Config::ENABLE_FOLDER_SIZE_PERCENTAGES),      true},
        };
        return table;
    }

    static const FieldSpec* findField(const std::string& key) {
        for (const auto& field : fieldTable()) {
            if (key == field.key) return &field;
        }
        return nullptr;
    }

    static std::vector<std::array<int, 3>> parseColorList(const std::string& firstLineValue, std::ifstream& in) {
        std::string block;

        if (firstLineValue.find('[') != std::string::npos) {
            block += firstLineValue.substr(firstLineValue.find('[') + 1);
            std::string line;

            while (std::getline(in, line)) {
                if (line.find(']') != std::string::npos) {
                    block += line.substr(0, line.find(']'));
                    break;
                }
                block += line;
            }
        }

        std::vector<std::array<int, 3>> colors;
        std::stringstream ss(block);
        std::string tuple;

        while (std::getline(ss, tuple, '}')) {
            size_t start = tuple.find('{');
            if (start == std::string::npos) continue;

            colors.push_back(parseColor(tuple.substr(start)));
        }

        return colors;
    }

    /* Reads a config file and populates a Config object:
    - Ignores empty lines and comments (#)
    - Splits lines by '=' into key/value
    - Trims leading/trailing spaces and tabs
    - Converts values to int, double, or string as appropriate
    - Special handling for CHART_COLORS array block
    */
    static std::optional<Config> parseConfigFile(const std::string& config_path) {
        std::ifstream in(config_path);
        if (!in.is_open()) return std::nullopt;

        Config cfg;
        std::string line;

        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#')
                continue;

            size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;

            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);

            trim(key);
            trim(value);

            try {
                if (key == "CHART_COLORS") {
                    // Multi-line array block; doesn't fit the single-value
                    // field table, so it's parsed separately.
                    cfg.CHART_COLORS = parseColorList(value, in);
                } else if (const FieldSpec* field = findField(key)) {
                    field->setter(cfg, value);
                }
                // Unknown keys are silently ignored, same as before.
            } catch (...) {
                return std::nullopt;
            }
        }

        return cfg;
    }
};
