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
        double duration_seconds{20.0};
        double base_pps_per_channel{120000.0};
        size_t packet_size_bytes{1400};
        std::string rate_steps{"0.50,0.75,1.00,1.10"};
        std::string output_json{};
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

    std::vector<double> parse_steps(const std::string &text)
    {
        std::vector<double> steps;
        std::stringstream ss(text);
        std::string token;
        while (std::getline(ss, token, ','))
        {
            if (token.empty())
            {
                continue;
            }
            steps.push_back(parse_double(token.c_str(), 1.0));
        }
        if (steps.empty())
        {
            return {0.50, 0.75, 1.00, 1.10};
        }
        return steps;
    }

    std::string default_output_path()
    {
        const auto ts = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
        return "stress_report_" + std::to_string(ts) + ".json";
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
            else if (arg == "--rate-steps" && i + 1 < argc)
            {
                options.rate_steps = argv[++i];
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
} // namespace

int main(int argc, char **argv)
{
    const Options options = parse_args(argc, argv);
    const std::vector<double> rate_steps = parse_steps(options.rate_steps);

    std::vector<benchsim::ScenarioResult> scenarios;

    {
        benchsim::ScenarioConfig cfg;
        cfg.name = "three_channel_full_load";
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

    for (double step : rate_steps)
    {
        benchsim::ScenarioConfig cfg;
        cfg.name = "rate_gradient_" + std::to_string(step);
        cfg.channels = 3;
        cfg.duration_seconds = std::max(5.0, options.duration_seconds * 0.5);
        cfg.target_pps_per_channel = options.base_pps_per_channel * step;
        cfg.channel_capacity_pps = options.base_pps_per_channel;
        cfg.packet_size_bytes = options.packet_size_bytes;
        cfg.cpi_fragments = 8;
        cfg.reassembly_queue_timeout_depth = 4096;
        cfg.numa_nodes = {0, 1, 1};
        scenarios.push_back(benchsim::run_scenario(cfg));
    }

    std::ostringstream report;
    report << "{\n";
    report << "  \"tool\": \"bench_stress\",\n";
    report << "  \"objective\": \"three-channel stress with rate gradients and core KPI collection\",\n";
    report << "  \"config\": {\n";
    report << "    \"duration_seconds\": " << options.duration_seconds << ",\n";
    report << "    \"base_pps_per_channel\": " << options.base_pps_per_channel << ",\n";
    report << "    \"packet_size_bytes\": " << options.packet_size_bytes << ",\n";
    report << "    \"rate_steps\": [";
    for (size_t i = 0; i < rate_steps.size(); ++i)
    {
        report << rate_steps[i];
        if (i + 1 < rate_steps.size())
        {
            report << ", ";
        }
    }
    report << "]\n";
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
