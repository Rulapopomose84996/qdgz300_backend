#include "bench_simulation.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    struct Options
    {
        double duration_seconds{12.0};
        double base_pps_per_channel{120000.0};
        size_t packet_size_bytes{1400};
        std::string output_json{};
        double loss_threshold{0.02};
        double cpi_success_threshold{0.98};
    };

    size_t parse_size(const char *value, size_t fallback)
    {
        if (value == nullptr)
        {
            return fallback;
        }
        char *end = nullptr;
        const unsigned long long parsed = std::strtoull(value, &end, 10);
        if (end == value || *end != '\0')
        {
            return fallback;
        }
        return static_cast<size_t>(parsed);
    }

    double parse_double(const char *value, double fallback)
    {
        if (value == nullptr)
        {
            return fallback;
        }
        char *end = nullptr;
        const double parsed = std::strtod(value, &end);
        if (end == value || *end != '\0')
        {
            return fallback;
        }
        return parsed;
    }

    std::string default_output_path()
    {
        const auto ts = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
        return "capacity_report_" + std::to_string(ts) + ".json";
    }

    Options parse_args(int argc, char **argv)
    {
        Options options;
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "--duration" && i + 1 < argc)
            {
                options.duration_seconds = parse_double(argv[++i], options.duration_seconds);
            }
            else if (arg == "--base-pps" && i + 1 < argc)
            {
                options.base_pps_per_channel = parse_double(argv[++i], options.base_pps_per_channel);
            }
            else if (arg == "--packet-size" && i + 1 < argc)
            {
                options.packet_size_bytes = parse_size(argv[++i], options.packet_size_bytes);
            }
            else if (arg == "--loss-threshold" && i + 1 < argc)
            {
                options.loss_threshold = parse_double(argv[++i], options.loss_threshold);
            }
            else if (arg == "--cpi-threshold" && i + 1 < argc)
            {
                options.cpi_success_threshold = parse_double(argv[++i], options.cpi_success_threshold);
            }
            else if (arg == "--output" && i + 1 < argc)
            {
                options.output_json = argv[++i];
            }
        }
        if (options.output_json.empty())
        {
            options.output_json = default_output_path();
        }
        return options;
    }

    bool pass_threshold(const benchsim::ScenarioResult &result,
                        double loss_threshold,
                        double cpi_success_threshold)
    {
        return result.loss_rate <= loss_threshold &&
               result.cpi_reassembly_success_rate >= cpi_success_threshold;
    }
} // namespace

int main(int argc, char **argv)
{
    const Options options = parse_args(argc, argv);
    std::vector<benchsim::ScenarioResult> scenarios;
    std::vector<benchsim::StaircaseProbe> staircase;

    {
        benchsim::ScenarioConfig cfg;
        cfg.name = "single_receiver_limit";
        cfg.channels = 1;
        cfg.duration_seconds = options.duration_seconds;
        cfg.target_pps_per_channel = options.base_pps_per_channel * 1.15;
        cfg.channel_capacity_pps = options.base_pps_per_channel;
        cfg.packet_size_bytes = options.packet_size_bytes;
        cfg.cpi_fragments = 8;
        cfg.reassembly_queue_timeout_depth = 2048;
        cfg.numa_nodes = {0};
        scenarios.push_back(benchsim::run_scenario(cfg));
    }

    {
        benchsim::ScenarioConfig cfg;
        cfg.name = "dual_receiver_concurrency_numa";
        cfg.channels = 2;
        cfg.duration_seconds = options.duration_seconds;
        cfg.target_pps_per_channel = options.base_pps_per_channel;
        cfg.channel_capacity_pps = options.base_pps_per_channel;
        cfg.packet_size_bytes = options.packet_size_bytes;
        cfg.cpi_fragments = 8;
        cfg.reassembly_queue_timeout_depth = 3072;
        cfg.numa_nodes = {0, 1};
        scenarios.push_back(benchsim::run_scenario(cfg));
    }

    {
        benchsim::ScenarioConfig cfg;
        cfg.name = "three_receiver_full_load";
        cfg.channels = 3;
        cfg.duration_seconds = options.duration_seconds;
        cfg.target_pps_per_channel = options.base_pps_per_channel;
        cfg.channel_capacity_pps = options.base_pps_per_channel;
        cfg.packet_size_bytes = options.packet_size_bytes;
        cfg.cpi_fragments = 8;
        cfg.reassembly_queue_timeout_depth = 4096;
        cfg.numa_nodes = {0, 1, 1};
        scenarios.push_back(benchsim::run_scenario(cfg));
    }

    double low = 0.50;
    double high = 1.50;
    double best = low;
    benchsim::ScenarioResult best_result;
    for (int round = 0; round < 8; ++round)
    {
        const double mid = (low + high) * 0.5;
        benchsim::ScenarioConfig cfg;
        cfg.name = "staircase_probe_round_" + std::to_string(round);
        cfg.channels = 3;
        cfg.duration_seconds = std::max(6.0, options.duration_seconds * 0.6);
        cfg.target_pps_per_channel = options.base_pps_per_channel * mid;
        cfg.channel_capacity_pps = options.base_pps_per_channel;
        cfg.packet_size_bytes = options.packet_size_bytes;
        cfg.cpi_fragments = 8;
        cfg.reassembly_queue_timeout_depth = 4096;
        cfg.numa_nodes = {0, 1, 1};

        const benchsim::ScenarioResult probe = benchsim::run_scenario(cfg);
        const bool pass = pass_threshold(probe, options.loss_threshold, options.cpi_success_threshold);
        staircase.push_back({mid, probe.measured_pps, probe.loss_rate, probe.cpi_reassembly_success_rate, pass});

        if (pass)
        {
            best = mid;
            best_result = probe;
            low = mid;
        }
        else
        {
            high = mid;
        }
    }

    best_result.name = "staircase_capacity_limit";
    scenarios.push_back(best_result);

    std::ostringstream report;
    report << "{\n";
    report << "  \"tool\": \"bench_capacity\",\n";
    report << "  \"objective\": \"capacity and bottleneck discovery for receiver paths\",\n";
    report << "  \"config\": {\n";
    report << "    \"duration_seconds\": " << options.duration_seconds << ",\n";
    report << "    \"base_pps_per_channel\": " << options.base_pps_per_channel << ",\n";
    report << "    \"packet_size_bytes\": " << options.packet_size_bytes << ",\n";
    report << "    \"loss_threshold\": " << options.loss_threshold << ",\n";
    report << "    \"cpi_success_threshold\": " << options.cpi_success_threshold << "\n";
    report << "  },\n";
    report << "  \"capacity_search\": {\n";
    report << "    \"best_rate_multiplier\": " << best << ",\n";
    report << "    \"binary_search_rounds\": " << staircase.size() << ",\n";
    report << "    \"probes\": [\n";
    for (size_t i = 0; i < staircase.size(); ++i)
    {
        const auto &p = staircase[i];
        report << "      {\"multiplier\": " << p.multiplier
               << ", \"measured_pps\": " << p.measured_pps
               << ", \"loss_rate\": " << p.loss_rate
               << ", \"cpi_success_rate\": " << p.cpi_success_rate
               << ", \"pass\": " << (p.pass ? "true" : "false") << "}";
        report << ((i + 1 < staircase.size()) ? ",\n" : "\n");
    }
    report << "    ]\n";
    report << "  },\n";
    report << "  \"scenarios\": [\n";
    for (size_t i = 0; i < scenarios.size(); ++i)
    {
        report << benchsim::scenario_to_json(scenarios[i], 4);
        report << ((i + 1 < scenarios.size()) ? ",\n" : "\n");
    }
    report << "  ]\n";
    report << "}\n";

    const std::string text = report.str();
    if (!benchsim::write_text_file(options.output_json, text))
    {
        std::cerr << "failed to write JSON report: " << options.output_json << "\n";
        return 1;
    }

    std::cout << text;
    std::cout << "report_path=" << options.output_json << "\n";
    return 0;
}
