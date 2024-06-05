#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <string_view>
#include <cstddef>
#include <algorithm>
#include <optional>
#include <variant>
#include <filesystem>
#include "alloc-inl.h"

extern "C" int run_target_wrap(void*, void*, int); // server, memory, length

using byte_array = std::vector<std::byte>;
using mask_array = std::vector<bool>;
using std::cout;
namespace fs = std::filesystem;

bool verbose = false;
std::vector<byte_array> input_testcases;

byte_array to_bytes(std::string_view str)
{
    byte_array result(str.size());
    for (std::size_t i = 0; i < str.size(); ++i)
        result[i] = static_cast<std::byte>(str[i]);
    return result;
}

std::string to_str(const byte_array& data)
{
    std::string result(data.size() + 1, '\0');
    for (std::size_t i = 0; i < data.size(); ++i)
        result[i] = static_cast<char>(data[i]);
    return result;
}

std::ostream& operator<<(std::ostream& out, const byte_array& data)
{
    return out << to_str(data);
}

void* server = nullptr;
bool crash_predicate(const byte_array& data)
{
    return run_target_wrap(server, (void*)data.data(), data.size());
}

// Edit distance implementation
// O(n^2) dynamic programming with path record
enum edit_kind { INS = 0, DEL, SUB };
struct data_t { std::size_t index; std::byte data; };
using edit = std::variant<data_t, std::size_t, data_t>;
using edit_trace = std::vector<edit>;

// convenient constructors
edit ins(std::size_t index, std::byte data)
{
    return edit{std::in_place_index<INS>, data_t{index, data}};
}
edit del(std::size_t index)
{
    return edit{std::in_place_index<DEL>, index};
}
edit sub(std::size_t index, std::byte data)
{
    return edit{std::in_place_index<SUB>, data_t{index, data}};
}

struct lookup_data_t
{
    std::size_t dist;
    std::optional<edit> last_edit;
    std::size_t last_row, last_col;
};

struct edit_distance_return_t
{
    std::size_t dist;
    edit_trace trace;
};

edit_trace get_trace(const std::vector<std::vector<lookup_data_t>>&, const byte_array&);
edit_distance_return_t edit_distance(const byte_array& from, const byte_array& to)
{
    // lookup[i][j] is "minimum distance to reach to[0..j-1] from from[0..i-1]"
    std::vector<std::vector<lookup_data_t>> lookup(from.size() + 1);
    for (auto& elem : lookup) elem.assign(to.size() + 1, {});

    // edge case
    lookup[0][0] = {0, std::nullopt, 0, 0};
    for (std::size_t i = 1; i <= from.size(); ++i)
        lookup[i][0] = {i, del(i - 1), i - 1, 0};
    for (std::size_t j = 1; j <= to.size(); ++j)
        lookup[0][j] = {j, ins(j - 1, to[j - 1]), 0, j - 1};

    for (std::size_t i = 1; i <= from.size(); ++i)
        for (std::size_t j = 1; j <= to.size(); ++j)
            if (from[i - 1] == to[j - 1])
                lookup[i][j] = {lookup[i - 1][j - 1].dist, std::nullopt, i - 1, j - 1};
            else
            {
                auto dist = std::min({
                    lookup[i][j - 1].dist, // insert
                    lookup[i - 1][j].dist, // delete
                    lookup[i - 1][j - 1].dist // substitute
                });
                if (dist == lookup[i][j - 1].dist)
                    lookup[i][j] = {dist + 1, ins(i, to[j - 1]), i, j - 1};
                else if (dist == lookup[i - 1][j].dist)
                    lookup[i][j] = {dist + 1, del(i - 1), i - 1, j};
                else lookup[i][j] = {dist + 1, sub(i - 1, to[j - 1]), i - 1, j - 1};
            }

    // print the lookup table
    if (verbose)
    {
        cout << "Lookup table:\n";
        for (int i = -1; i <= static_cast<int>(from.size()); ++i)
        {
            if (i <= 0) cout << " ";
            else cout << static_cast<char>(from[i - 1]);
            for (std::size_t j = 0; j <= to.size(); ++j)
            {
                cout << " ";
                if (i == -1)
                {
                    if (j == 0) cout << " ";
                    else cout << static_cast<char>(to[j - 1]);
                }
                else cout << lookup[i][j].dist;
            }
            cout << "\n";
        }
    }

    return {lookup[from.size()][to.size()].dist, get_trace(lookup, to)};
}

edit_trace get_trace(const std::vector<std::vector<lookup_data_t>>& lookup, const byte_array& dest)
{
    edit_trace result;
    std::size_t cur_row = lookup.size() - 1, cur_col = lookup[0].size() - 1;
    while (cur_row > 0 || cur_col > 0)
    {
        const auto& [dist, edit, row, col] = lookup[cur_row][cur_col];
        if (edit)
            result.push_back(*edit);
        cur_row = row; cur_col = col;
    }
    return result;
}

// printer for edit and trace
std::ostream& operator<<(std::ostream& out, std::byte byte)
{
    std::ios_base::fmtflags f{out.flags()};
    out << std::hex << "0x" << static_cast<unsigned int>(byte) << "('" << static_cast<char>(byte) << "')";
    out.flags(f);
    return out;
}
std::ostream& operator<<(std::ostream& out, edit single_edit)
{
    if (single_edit.index() == INS)
    {
        auto [index, data] = std::get<INS>(single_edit);
        out << "Ins(" << index << ", " << data << ")";
    }
    else if (single_edit.index() == DEL)
    {
        auto index = std::get<DEL>(single_edit);
        out << "Del(" << index << ")";
    }
    else if (single_edit.index() == SUB)
    {
        auto [index, data] = std::get<SUB>(single_edit);
        out << "Sub(" << index << ", " << data << ")";
    }
    return out;
}
std::ostream& operator<<(std::ostream& out, const edit_trace& trace)
{
    out << "[";
    auto first = true;
    for (auto elem : trace)
    {
        if (first) first = false;
        else out << ", ";
        out << elem;
    }
    out << "]";
    return out;
}

// apply edits
void apply_edit(edit single_edit, byte_array& orig, bool print = false)
{
    if (orig.empty()) return;
    auto preserve = orig;
    if (single_edit.index() == INS)
    {
        auto [index, data] = std::get<INS>(single_edit);
        if (index >= orig.size()) index = orig.size();
        orig.insert(orig.begin() + index, data);
        if (print) cout << "Insert " << data << " at " << index;
    }
    else if (single_edit.index() == DEL)
    {
        auto index = std::get<DEL>(single_edit);
        if (index >= orig.size()) index = orig.size() - 1;
        orig.erase(orig.begin() + index);
        if (print) cout << "Delete " << index;
    }
    else if (single_edit.index() == SUB)
    {
        auto [index, data] = std::get<SUB>(single_edit);
        if (index >= orig.size()) index = orig.size() - 1;
        orig[index] = data;
        if (print) cout << "Replace " << data << " at " << index;
    }
    if (print) cout << " (" << preserve << " -> " << orig << ")";
}
byte_array apply_edits(const edit_trace& trace, const byte_array& orig, const mask_array& mask = {}, bool print = false)
{
    auto result = orig;
    if (print) cout << "Total edits: " << trace.size() << " (" << mask.size() << " masked)\n";
    for (std::size_t i = 0; i < trace.size(); ++i)
    {
        if (i < mask.size() && !mask[i]) continue;
        if (print) cout << "[" << i << "] ";
        apply_edit(trace[i], result, print);
        if (print) cout << "\n";
    }
    return result;
}

// Delta debugging parts
// Random partition the edits into k chunks
std::optional<mask_array> test_partitions(edit_trace& trace, const byte_array& orig, std::size_t parts)
{
    auto part_size = trace.size() / parts;
    auto remainder = trace.size() % parts;
    std::size_t start = 0;
    while (start < trace.size())
    {
        auto end = start + part_size;
        if (remainder > 0)
        {
            --remainder;
            ++end;
        }
        mask_array mask(trace.size(), false);
        for (auto j = start; j < end; ++j) mask[j] = true;
        auto result = apply_edits(trace, orig, mask);
        if (verbose) cout << "Mask(" << start << "-" << end - 1 << ") = " << result << "\n";
        if (crash_predicate(result))
        {
            if (!verbose) cout << "Mask(" << start << "-" << end - 1 << ") = (length " << result.size() << ")\n";
            return mask;
        }

        // Also test the complement
        auto compl_mask = mask;
        for (std::size_t j = 0; j < compl_mask.size(); ++j)
            compl_mask[j] = !compl_mask[j];
        auto compl_result = apply_edits(trace, orig, compl_mask);
        if (verbose) cout << "~Mask(" << start << "-" << end - 1 << ") = " << compl_result << "\n";
        if (crash_predicate(compl_result))
        {
            if (!verbose) cout << "~Mask(" << start << "-" << end - 1 << ") = (length " << compl_result.size() << ")\n";
            return compl_mask;
        }
        start = end;
    }
    cout << "Failed!\n";
    return {};
}
edit_trace delta_edit(const edit_trace& trace, const byte_array& orig)
{
    std::size_t parts = 2;
    auto cur_trace = trace;
    while (cur_trace.size() > 1 && parts <= 2 * cur_trace.size())
    {
        cout << "Trying " << parts << " partitions...\n";
        auto mask_result = test_partitions(cur_trace, orig, parts);
        if (mask_result)
        {
            auto& mask = *mask_result;
            edit_trace new_trace;
            for (std::size_t i = 0; i < cur_trace.size(); ++i)
            {
                if (i < mask.size() && !mask[i]) continue;
                new_trace.push_back(cur_trace[i]);
            }
            if (verbose) cout << "Success!\nOriginal: " << cur_trace << "\nMasked: " << new_trace << "\n";
            else cout << "Success! Edit length reduced from " << cur_trace.size() << " to " << new_trace.size() << "\n";
            cur_trace = new_trace;
            parts = 2;
        }
        else parts *= 2;
    }
    return cur_trace;
}

byte_array find_closest_initial(const byte_array& crash)
{
    if (input_testcases.empty()) throw std::logic_error("No testcases specified!");
    std::size_t min_dist = static_cast<std::size_t>(-1);
    const byte_array* ptr_max{nullptr};
    for (const auto& testcase : input_testcases)
    {
        auto [dist, _] = edit_distance(testcase, crash);
        if (dist < min_dist)
        {
            min_dist = dist;
            ptr_max = &testcase;
        }
    }
    return *ptr_max;
}

extern "C" void entry_point(void* fsrv, void** mem, int* len_ptr)
{
    server = fsrv;
    byte_array crash(*len_ptr);
    auto ptr = (std::byte*)*mem;
    for (int i = 0; i < *len_ptr; ++i) crash[i] = ptr[i];
    auto orig = find_closest_initial(crash);
    // auto crash = to_bytes("g;odbye");
    if (verbose)
    {
        cout << "Original test case: " << orig << "\n";
        cout << "Original crash: " << crash << "\n";
    }
    else
    {
        cout << "Original test case length: " << orig.size() << "\n";
        cout << "Original crash length: " << crash.size() << "\n";
    }

    auto [dist, trace] = edit_distance(orig, crash);
    cout << "Original distance: " << dist << "\n";
    auto result = apply_edits(trace, orig, {}, verbose);
    if (verbose) cout << "Edit result: " << result << "\n";
    else cout << "Edit result length: " << result.size() << "\n";

    cout << "==========\n";
    auto new_trace = delta_edit(trace, orig);
    cout << "Optimal distance: " << new_trace.size() << "\n";
    auto result2 = apply_edits(new_trace, orig, {}, verbose);
    if (verbose) cout << "Optimal result: " << result2 << "\n";
    else cout << "Optimal result length: " << result2.size() << "\n";

    // Return the memory and reallocate another one
    mem = ck_realloc(*mem, result2.size());
    ptr = (std::byte*)*mem;
    *len_ptr = result2.size();
    for (int i = 0; i < result2.size(); ++i) ptr[i] = result2[i];
}

void add_testcase(const fs::path& path)
{
    std::ifstream file_in{path, std::ios::in | std::ios::binary};
    file_in.unsetf(std::ios::skipws); // don't skip newline

    // get file size
    file_in.seekg(0, std::ios::end);
    std::streampos file_size = file_in.tellg();
    file_in.seekg(0, std::ios::beg);

    // read
    byte_array contents(file_size);
    file_in.read(reinterpret_cast<char*>(contents.data()), file_size);
    cout << "Successfully read " << file_size << " bytes from " << path << "\n";
    input_testcases.push_back(std::move(contents));
}

extern "C" void read_testcase_dir(void* dir_ptr)
{
    input_testcases.clear();
    if (dir_ptr == nullptr)
    {
        cout << "Warning: No test case specified. Use \"hello\" as default test case.\n";
        input_testcases.push_back(to_bytes("hello"));
        return;
    }
    std::string dir_str(static_cast<char*>(dir_ptr));
    fs::path dir_path(std::move(dir_str));
    if (!fs::exists(dir_path))
    {
        cout << "Error! The given input path " << dir_path << " does not exist!\n";
        throw std::logic_error("Path does not exist");
    }
    if (fs::is_directory(dir_path))
    {
        for (const auto& entry : fs::directory_iterator{dir_path})
            if (fs::is_regular_file(entry))
                add_testcase(entry);
    }
    else if (fs::is_regular_file(dir_path)) add_testcase(dir_path);
    cout << "Read " << input_testcases.size() << " test cases.\n";
}
