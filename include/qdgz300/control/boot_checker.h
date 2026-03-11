#pragma once

#include <functional>
#include <string>
#include <vector>

namespace qdgz300::control
{
    class BootChecker
    {
    public:
        struct CheckResult
        {
            std::string name{};
            bool passed{false};
            std::string detail{};
        };

        using Check = std::function<CheckResult()>;

        void add_check(Check check);
        std::vector<CheckResult> run_all();
        bool all_passed() const noexcept { return all_passed_; }

    private:
        std::vector<Check> checks_;
        bool all_passed_{false};
    };
}
