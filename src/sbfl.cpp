#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <unordered_map>
#include <cstddef>
#include <cmath>
#include <stdexcept>
#include <algorithm>
#include <numeric>

using byte_array = std::vector<std::byte>;
using std::cout;

static constexpr int show_top = 10;

class SBFL
{
private:
    struct stats { std::size_t e_f = 0, n_f = 0, e_p = 0, n_p = 0; };
    std::vector<stats> cur_stats;
    std::size_t num_passed = 0, num_failed = 0;
    using method_func_ptr = double (*)(stats);
    static const std::unordered_map<std::string, method_func_ptr> methods;

public:
    void add_coverage(const byte_array& coverage, bool is_crash);
    std::vector<double> get_result(std::string_view mode) const;
};

const std::unordered_map<std::string, SBFL::method_func_ptr> SBFL::methods = {
    {"ochiai", [](stats s) { return s.e_f / std::sqrt((s.e_f + s.n_f) * (s.e_f + s.e_p)); }},
    {"dstar", [](stats s) { return static_cast<double>(s.e_f * s.e_f) / (s.e_p + s.n_f); }}
};

void SBFL::add_coverage(const byte_array& coverage, bool is_crash)
{
    if (coverage.size() > cur_stats.size())
    {
        // Map expansion
        auto old_size = cur_stats.size();
        cur_stats.resize(coverage.size());
        for (auto i = old_size; i < cur_stats.size(); ++i)
        {
            cur_stats[i].n_f = num_failed;
            cur_stats[i].n_p = num_passed;
        }
    }
    for (int i = 0; i < coverage.size(); ++i)
        if (coverage[i] == std::byte{0})
        {
            // not executed
            if (is_crash) ++cur_stats[i].n_f;
            else ++cur_stats[i].n_p;
        }
        else
        {
            // executed
            if (is_crash) ++cur_stats[i].e_f;
            else ++cur_stats[i].e_p;
        }
    if (is_crash) ++num_failed;
    else ++num_passed;
}

std::vector<double> SBFL::get_result(std::string_view mode) const
{
    if (num_failed < 5) cout << "Warning: not enough failed test logged!\n";
    std::vector<double> result(cur_stats.size());
    auto it = methods.find(std::string{mode});
    if (it == methods.end())
        throw std::logic_error("Unknown method: " + std::string{mode});
    for (int i = 0; i < cur_stats.size(); ++i)
        result[i] = it->second(cur_stats[i]);
    return result;
}

SBFL& global_sbfl()
{
    // Meyers singleton
    static SBFL instance;
    return instance;
}

void add_coverage(const byte_array& coverage, bool is_crash)
{
    global_sbfl().add_coverage(coverage, is_crash);
}

void display_result(std::string_view mode)
{
    auto result = global_sbfl().get_result(mode);
    std::vector<std::size_t> indices(result.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::partial_sort(
        indices.begin(), indices.begin() + show_top, indices.end(),
        [&result](std::size_t a, std::size_t b) { return result[a] > result[b]; }
    );
    for (int i = 0; i < show_top; ++i)
        cout << "#" << i << ": " << std::hex << "0x" << indices[i] << std::dec << " (" << result[indices[i]] << ")\n";
}
