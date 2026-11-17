#include "flamegraph_html.h"

#include "common.h"
#include "flamegraph_assets.h"

#include <string>
#include <string_view>

namespace wperf::detail {
namespace {

std::wstring EscapeHtml(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (wchar_t ch : value) {
        switch (ch) {
        case L'&': result += L"&amp;"; break;
        case L'<': result += L"&lt;"; break;
        case L'>': result += L"&gt;"; break;
        case L'"': result += L"&quot;"; break;
        case L'\'': result += L"&#39;"; break;
        default: result += ch; break;
        }
    }
    return result;
}

std::string EscapeJsonForHtml(std::string value) {
    std::string result;
    result.reserve(value.size());
    for (char ch : value) {
        if (ch == '<') result += "\\u003c";
        else if (ch == '>') result += "\\u003e";
        else if (ch == '&') result += "\\u0026";
        else result += ch;
    }
    return result;
}

}  // namespace

std::wstring RenderFlameGraphHtml(std::string data_json,
                                  const FlameGraphOptions& options,
                                  uint64_t total_weight,
                                  uint64_t stacks,
                                  uint64_t nodes) {
    const std::wstring title =
        options.title.empty() ? L"wperf flame graph" : options.title;
    const std::wstring weight_name =
        options.weight_name.empty() ? L"samples" : options.weight_name;

    std::wstring chips;
    for (const auto& [label, value] : options.details) {
        if (label.empty() && value.empty()) continue;
        chips += L"<span class=\"chip\"><b>" + EscapeHtml(label) + L"</b> " +
                 EscapeHtml(value) + L"</span>";
    }
    chips += L"<span class=\"chip\"><b>stacks</b> " +
             std::to_wstring(stacks) + L"</span><span class=\"chip\"><b>nodes</b> " +
             std::to_wstring(nodes) + L"</span>";

    const std::wstring source = options.source.empty()
        ? L"<p class=\"subtitle\">Collapsed-stack input</p>"
        : L"<p class=\"subtitle\">Source: " + EscapeHtml(options.source) + L"</p>";

    std::wstring html;
    html.reserve(data_json.size() + 24000);
    html += L"<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
            L"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
            L"<title>" + EscapeHtml(title) + L"</title>\n<style>" +
            kFlameGraphStyle + L"</style>\n</head>\n<body>\n<main class=\"app\">\n"
            L"<header class=\"hero\"><div><div class=\"eyebrow\">wperf / flame graph</div><h1>" +
            EscapeHtml(title) + L"</h1>" + source +
            L"</div><div class=\"hero-metric\"><span>Total weight</span><strong id=\"total-value\">" +
            std::to_wstring(total_weight) + L" " + EscapeHtml(weight_name) +
            L"</strong></div></header>\n"
            L"<section class=\"stats-strip\">" + chips + L"</section>\n"
            L"<section class=\"toolbar card\">"
            L"<div class=\"search-field\"><input id=\"search\" type=\"search\" placeholder=\"Search stacks, modules, symbols (Enter / Shift+Enter)\" aria-label=\"Search frames\"><span id=\"search-counter\"></span></div>"
            L"<div class=\"tool-group\"><button id=\"prev-match\" class=\"icon-button\" type=\"button\" title=\"Previous search match\">↑</button>"
            L"<button id=\"next-match\" class=\"icon-button\" type=\"button\" title=\"Next search match\">↓</button>"
            L"<button id=\"up\" type=\"button\">Up</button><button id=\"reset\" type=\"button\">Reset</button></div>"
            L"<label class=\"filter-control\">Min % <input id=\"min-percent\" type=\"range\" min=\"0\" max=\"5\" step=\"0.05\" value=\"0\"><output id=\"min-percent-value\">0%</output></label>"
            L"<div class=\"tool-group\"><button id=\"download-svg\" type=\"button\">Download SVG</button><button id=\"download-png\" type=\"button\">Download PNG</button></div>"
            L"</section>\n<div class=\"workspace\">\n"
            L"<section class=\"chart-card card\"><div class=\"chart-head\"><div><h2 class=\"chart-title\">Flame graph</h2><span class=\"chart-status\" id=\"visible-status\"></span></div><nav class=\"breadcrumb\" id=\"breadcrumb\" aria-label=\"Flame graph focus path\"></nav></div>"
            L"<div class=\"chart-stage\"><svg id=\"chart\" role=\"img\" aria-label=\"Interactive flame graph. Click to inspect, double-click to zoom, drag horizontally to select a range.\"></svg></div>"
            L"<div class=\"chart-hints\">Click to inspect · double-click to zoom · drag to select a weight range · Backspace / Up navigate outward · Esc resets</div></section>\n"
            L"<aside class=\"details-card card\"><div class=\"details-head\"><div><div class=\"eyebrow\">Selected frame</div><h2 class=\"details-title\" id=\"details-name\">No frame selected</h2><div class=\"details-module\" id=\"details-module\"></div><details class=\"details-meta\" id=\"details-meta\" hidden><summary>Module identity</summary><code id=\"details-identity\"></code></details></div><div class=\"details-actions\"><button id=\"zoom-selected\" type=\"button\" disabled>Zoom</button></div></div>"
            L"<div class=\"metric-grid\"><div class=\"metric\"><span>Inclusive</span><strong id=\"details-inclusive\">—</strong><small id=\"details-inclusive-unit\"></small></div><div class=\"metric\"><span>Self</span><strong id=\"details-self\">—</strong><small id=\"details-self-unit\"></small></div><div class=\"metric\"><span>Share</span><strong id=\"details-share\">—</strong><small></small></div></div>"
            L"<div class=\"stack-head\"><h3>Call stack</h3><div class=\"stack-actions\"><button id=\"copy-stack\" type=\"button\" disabled>Copy stack</button><button id=\"copy-folded\" type=\"button\" disabled>Copy folded</button></div></div>"
            L"<pre class=\"details-stack\" id=\"details-stack\"></pre><div class=\"empty-details\" id=\"details-empty\">Click a frame to inspect its self/inclusive weight and full stack.</div>"
            L"<p class=\"hint\">Stack copy uses one frame per line. Folded copy uses <code>;</code>-separated input with the current weight.</p></aside>\n"
            L"</div><footer><span>Self-contained local HTML · no network dependencies</span><span>Generated by wperf</span></footer>\n"
            L"</main><div id=\"tooltip\"></div>\n"
            L"<script type=\"application/json\" id=\"flame-data\">" +
            ToUtf16(EscapeJsonForHtml(std::move(data_json))) + L"</script>\n<script>" +
            std::wstring(kFlameGraphScript) + L"</script>\n</body>\n</html>\n";
    return html;
}

}  // namespace wperf::detail
