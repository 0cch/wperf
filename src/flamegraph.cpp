#include "flamegraph.h"

#include "common.h"
#include "flamegraph_html.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cwctype>
#include <map>
#include <string>
#include <vector>

namespace wperf {
namespace {

struct Node {
    std::wstring name;
    uint64_t value = 0;
    uint64_t self = 0;
    std::map<std::wstring, Node> children;
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

bool ParseFolded(std::wstring_view folded, Node& root, uint64_t& total_weight,
                 uint64_t& stacks, uint64_t& nodes, std::wstring* error) {
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
        if (!AddWeight(total_weight, weight) || !AddWeight(root.value, weight)) {
            if (error != nullptr) *error = L"folded weight total overflows uint64";
            return false;
        }
        ++stacks;

        Node* node = &root;
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
                if (error != nullptr) *error = L"flame graph depth exceeds 4096 frames";
                return false;
            }
            auto found = node->children.find(frame);
            if (found == node->children.end()) {
                if (nodes >= kMaxFlameGraphNodes) {
                    if (error != nullptr) *error = L"flame graph exceeds 500000 distinct nodes";
                    return false;
                }
                found = node->children.emplace(frame, Node{}).first;
                found->second.name = frame;
                ++nodes;
            }
            node = &found->second;
            if (!AddWeight(node->value, weight)) {
                if (error != nullptr) *error = L"folded stack weight overflows uint64";
                return false;
            }
            if (frame_end == std::wstring::npos) break;
            frame_start = frame_end + 1;
        }
        if (!AddWeight(node->self, weight)) {
            if (error != nullptr) *error = L"folded leaf weight overflows uint64";
            return false;
        }
        if (end == folded.size()) break;
    }
    return true;
}

void NodeJson(const Node& node, nlohmann::ordered_json& value) {
    value["name"] = ToUtf8(node.name);
    value["value"] = node.value;
    value["self"] = node.self;
    std::vector<const Node*> children;
    children.reserve(node.children.size());
    for (const auto& [name, child] : node.children) {
        (void)name;
        children.push_back(&child);
    }
    std::sort(children.begin(), children.end(), [](const Node* left, const Node* right) {
        if (left->value != right->value) return left->value > right->value;
        return left->name < right->name;
    });
    nlohmann::ordered_json child_values = nlohmann::ordered_json::array();
    for (const Node* child : children) {
        nlohmann::ordered_json child_value;
        NodeJson(*child, child_value);
        child_values.push_back(std::move(child_value));
    }
    value["children"] = std::move(child_values);
}

}  // namespace

bool RenderFoldedFlameGraph(std::wstring_view folded,
                            const FlameGraphOptions& options,
                            FlameGraphResult& result,
                            std::wstring* error) {
    if (!folded.empty() && folded.front() == L'\ufeff') folded.remove_prefix(1);

    Node root;
    root.name = L"root";
    uint64_t total_weight = 0;
    uint64_t stacks = 0;
    uint64_t nodes = 0;
    if (!ParseFolded(folded, root, total_weight, stacks, nodes, error)) return false;
    if (total_weight == 0) {
        if (error != nullptr) *error = L"folded input contains no positive weight";
        return false;
    }

    nlohmann::ordered_json data;
    NodeJson(root, data);
    data["weight_name"] = ToUtf8(options.weight_name.empty() ? L"samples" : options.weight_name);

    result.html = detail::RenderFlameGraphHtml(
        data.dump(), options, total_weight, stacks, nodes);
    result.total_weight = total_weight;
    result.stacks = stacks;
    result.nodes = nodes;
    return true;
}

}  // namespace wperf
