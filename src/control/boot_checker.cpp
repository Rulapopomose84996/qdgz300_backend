#include "qdgz300/control/boot_checker.h"

namespace qdgz300::control
{
    void BootChecker::add_check(Check check)
    {
        checks_.push_back(std::move(check));
    }

    std::vector<BootChecker::CheckResult> BootChecker::run_all()
    {
        std::vector<CheckResult> results;
        all_passed_ = true;

        for (const auto &check : checks_)
        {
            const auto result = check();
            all_passed_ = all_passed_ && result.passed;
            results.push_back(result);
        }

        return results;
    }
}
