#include "flamegraph.h"

#include "common.h"
#include "flamegraph_html.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cwctype>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace wperf {
namespace {

struct Node {
    std::wstring name;
    uint64_t value = 0;
    uint64_t self = 0;
    std::map<std::wstring, size_t> children;
};

struct Tree {
    std::vector<Node> nodes;
};

constexpr uint64_t kMaxFlameGraphNodes = 500000;
constexpr size_t kMaxFlameGraphDepth = 4096;

std::wstring Trim(std::wstring_view value) {
    size_t begin = 0;
    while (begin < value.size() && std::iswspace(value[begin])) ++begin;
    size_t end = value.size();
    while (end > begin && std::iswspace(value[end - 1])) --end;
    return std::wstring(value.substr(begin, end - begin));
}

bool AddWeight(uint64_t& target, uint64_t value) {
    if (value > UINT64_MAX - target) return false;
    target += value;
    return true;
}

bool ParseWeight(const std::wstring& text, uint64_t& value) {
    if (text.empty() || text[0] == L'-') return false;
    errno = 0;
    wchar_t* end = nullptr;
    const unsigned long long parsed = wcstoull(text.c_str(), &end, 10);
    if (errno == ERANGE || end == nullptr || *end != L'\0') return false;
    value = static_cast<uint64_t>(parsed);
    return true;
}

bool ParseFolded(std::wstring_view folded, Tree& tree, uint64_t& total_weight,
                 uint64_t& stacks, uint64_t& nodes, std::wstring* error) {
    tree.nodes.clear();
    tree.nodes.push_back(Node{L"root", 0, 0, {}});

    size_t line_number = 0;
    size_t position = 0;
    while (position <= folded.size()) {
        ++line_number;
        size_t end = folded.find(L'\n', position);
        if (end == std::wstring_view::npos) end = folded.size();
        std::wstring line = Trim(folded.substr(position, end - position));
        position = end + 1;
        if (line.empty() || line[0] == L'#') {
            if (end == folded.size()) break;
            continue;
        }

        const size_t split = line.find_last_of(L" \t");
        if (split == std::wstring::npos) {
            if (error != nullptr) {
                *error = L"folded line " + std::to_wstring(line_number) +
                         L" has no weight field";
            }
            return false;
        }
        std::wstring stack_text = Trim(line.substr(0, split));
        std::wstring weight_text = Trim(line.substr(split + 1));
        uint64_t weight = 0;
        if (stack_text.empty() || !ParseWeight(weight_text, weight)) {
            if (error != nullptr) {
                *error = L"folded line " + std::to_wstring(line_number) +
                         L" does not contain a valid stack and integer weight";
            }
            return false;
        }
        if (!AddWeight(total_weight, weight) || !AddWeight(tree.nodes[0].value, weight)) {
            if (error != nullptr) *error = L"folded weight total overflows uint64";
            return false;
        }
        ++stacks;

        size_t node_index = 0;
        size_t frame_start = 0;
        size_t frame_count = 0;
        while (frame_start <= stack_text.size()) {
            const size_t frame_end = stack_text.find(L';', frame_start);
            const std::wstring frame = Trim(stack_text.substr(
                frame_start, frame_end == std::wstring::npos
                                 ? std::wstring::npos
                                 : frame_end - frame_start));
            if (frame.empty()) {
                if (error != nullptr) {
                    *error = L"folded line " + std::to_wstring(line_number) +
                             L" contains an empty frame";
                }
                return false;
            }
            if (++frame_count > kMaxFlameGraphDepth) {
                if (error != nullptr) {
                    *error = L"flame graph depth exceeds " +
                             std::to_wstring(kMaxFlameGraphDepth) + L" frames";
                }
                return false;
            }

            auto& parent = tree.nodes[node_index];
            auto found = parent.children.find(frame);
            if (found == parent.children.end()) {
                if (nodes >= kMaxFlameGraphNodes) {
                    if (error != nullptr) {
                        *error = L"flame graph exceeds " +
                                 std::to_wstring(kMaxFlameGraphNodes) +
                                 L" distinct nodes";
                    }
                    return false;
                }
                const size_t child_index = tree.nodes.size();
                parent.children.emplace(frame, child_index);
                tree.nodes.push_back(Node{frame, 0, 0, {}});
                ++nodes;
                node_index = child_index;
            } else {
                node_index = found->second;
            }

            if (!AddWeight(tree.nodes[node_index].value, weight)) {
                if (error != nullptr) *error = L"folded stack weight overflows uint64";
                return false;
            }
            if (frame_end == std::wstring::npos) break;
            frame_start = frame_end + 1;
        }
        if (!AddWeight(tree.nodes[node_index].self, weight)) {
            if (error != nullptr) *error = L"folded leaf weight overflows uint64";
            return false;
        }
        if (end == folded.size()) break;
    }
    return true;
}

std::vector<size_t> SortedChildren(const Tree& tree, const Node& node) {
    std::vector<size_t> children;
    children.reserve(node.children.size());
    for (const auto& [name, index] : node.children) {
        (void)name;
        children.push_back(index);
    }
    std::sort(children.begin(), children.end(), [&](size_t left_index, size_t right_index) {
        const Node& left = tree.nodes[left_index];
        const Node& right = tree.nodes[right_index];
        if (left.value != right.value) return left.value > right.value;
        return left.name < right.name;
    });
    return children;
}

void AppendNodeStart(std::string& output, const Node& node) {
    output += "{\"name\":";
    output += nlohmann::ordered_json(ToUtf8(node.name)).dump();
    output += ",\"value\":\"";
    output += std::to_string(node.value);
    output += "\",\"self\":\"";
    output += std::to_string(node.self);
    output += "\",\"children\":[";
}

std::string SerializeTree(const Tree& tree, std::wstring_view weight_name) {
    struct Frame {
        size_t node_index = 0;
        std::vector<size_t> children;
        size_t next_child = 0;
        bool started = false;
    };

    std::string output;
    output.reserve(tree.nodes.size() * 64);
    std::vector<Frame> stack;
    stack.push_back(Frame{0, SortedChildren(tree, tree.nodes[0]), 0, false});
    while (!stack.empty()) {
        Frame& frame = stack.back();
        if (!frame.started) {
            AppendNodeStart(output, tree.nodes[frame.node_index]);
            frame.started = true;
        }
        if (frame.next_child < frame.children.size()) {
            if (frame.next_child != 0) output.push_back(',');
            const size_t child_index = frame.children[frame.next_child++];
            AppendNodeStart(output, tree.nodes[child_index]);
            stack.push_back(Frame{child_index, SortedChildren(tree, tree.nodes[child_index]), 0, true});
            continue;
        }

        output += "]}";
        if (stack.size() == 1) {
            output.resize(output.size() - 1);
            output += ",\"weight_name\":";
            output += nlohmann::ordered_json(ToUtf8(weight_name)).dump();
            output += "}";
        }
        stack.pop_back();
    }
    return output;
}

}  // namespace

bool RenderFoldedFlameGraph(std::wstring_view folded,
                            const FlameGraphOptions& options,
                            FlameGraphResult& result,
                            std::wstring* error) {
    if (!folded.empty() && folded.front() == L'\ufeff') folded.remove_prefix(1);

    Tree tree;
    uint64_t total_weight = 0;
    uint64_t stacks = 0;
    uint64_t nodes = 0;
    if (!ParseFolded(folded, tree, total_weight, stacks, nodes, error)) return false;
    if (total_weight == 0) {
        if (error != nullptr) *error = L"folded input contains no positive weight";
        return false;
    }

    const std::wstring_view weight_name =
        options.weight_name.empty() ? L"samples" : std::wstring_view(options.weight_name);
    std::string data_json = SerializeTree(tree, weight_name);

    result.html = detail::RenderFlameGraphHtml(
        std::move(data_json), options, total_weight, stacks, nodes);
    result.total_weight = total_weight;
    result.stacks = stacks;
    result.nodes = nodes;
    return true;
}

}  // namespace wperf
