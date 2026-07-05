// bench_json.cpp — real-world single-header-library TU: nlohmann/json v3.11.3.
#include "json.hpp"

#include <iostream>
#include <string>
#include <vector>

using json = nlohmann::json;

struct Config {
    std::string name;
    int retries = 0;
    double timeout = 0.0;
    std::vector<std::string> tags;
};

static void to_json(json& j, const Config& c) {
    j = json{{"name", c.name},
             {"retries", c.retries},
             {"timeout", c.timeout},
             {"tags", c.tags}};
}

static void from_json(const json& j, Config& c) {
    j.at("name").get_to(c.name);
    j.at("retries").get_to(c.retries);
    j.at("timeout").get_to(c.timeout);
    j.at("tags").get_to(c.tags);
}

int main() {
    json doc = json::parse(R"({
        "service": "bench",
        "configs": [
            {"name": "a", "retries": 3, "timeout": 1.5, "tags": ["x", "y"]},
            {"name": "b", "retries": 0, "timeout": 0.25, "tags": []}
        ],
        "enabled": true,
        "weights": [1, 2, 3, 4]
    })");

    std::vector<Config> configs = doc["configs"].get<std::vector<Config>>();

    int total_retries = 0;
    for (const auto& c : configs) total_retries += c.retries;

    json out;
    out["count"] = configs.size();
    out["total_retries"] = total_retries;
    out["echo"] = configs;
    out["sum_weights"] = 0;
    for (const auto& w : doc["weights"]) {
        out["sum_weights"] = out["sum_weights"].get<int>() + w.get<int>();
    }

    std::string dumped = out.dump(2);
    std::cout << "bench_json ok: " << dumped.size() << " bytes\n";
    return dumped.empty() ? 1 : 0;
}
