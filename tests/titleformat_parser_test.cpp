// SPDX-License-Identifier: GPL-3.0-only

#include "test_checks.hpp"
#include "trackknife/titleformat/parser.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace {

using checks::failures;

template <typename NodeType>
[[nodiscard]] const NodeType* nodeAs(const trackknife::titleformat::SyntaxTree& tree,
                                     const trackknife::titleformat::NodeId id) {
    return std::get_if<NodeType>(&tree.node(id).data);
}

[[nodiscard]] const trackknife::titleformat::SequenceNode*
rootSequence(const trackknife::titleformat::SyntaxTree& tree) {
    return nodeAs<trackknife::titleformat::SequenceNode>(tree, tree.root());
}

void parsesNestedSourcePreservingly() {
    using namespace trackknife::titleformat;
    const std::string source = "%artist% - $if(%date%,\\(%album%\\),unknown)";
    const auto output = parse(source);
    CHECK(output.isValid());
    CHECK(output.canExecute());
    CHECK(output.tree.source() == source);

    const auto* root = rootSequence(output.tree);
    CHECK(root != nullptr);
    CHECK(root && root->children.size() == 3U);
    if (root == nullptr || root->children.size() != 3U) {
        return;
    }
    const auto* field = nodeAs<FieldNode>(output.tree, root->children.at(0));
    CHECK(field && field->name == "artist");
    const auto* call = nodeAs<CallNode>(output.tree, root->children.at(2));
    CHECK(call && call->name == "if");
    CHECK(call && call->arguments.size() == 3U);
}

void escapedSyntaxStaysLiteral() {
    using namespace trackknife::titleformat;
    const auto output = parse("$replace(%title%,\\,,\\$\\%\\(\\)\\\\)");
    CHECK(output.isValid());
    const auto* root = rootSequence(output.tree);
    CHECK(root && root->children.size() == 1U);
    if (root == nullptr || root->children.empty()) {
        return;
    }
    const auto* call = nodeAs<CallNode>(output.tree, root->children.front());
    CHECK(call && call->arguments.size() == 3U);
    if (call == nullptr || call->arguments.size() != 3U) {
        return;
    }
    const auto* separator = nodeAs<SequenceNode>(output.tree, call->arguments.at(1));
    const auto* replacement = nodeAs<SequenceNode>(output.tree, call->arguments.at(2));
    CHECK(separator && separator->children.size() == 1U);
    CHECK(replacement && replacement->children.size() == 1U);
    if (separator && !separator->children.empty()) {
        const auto* literal = nodeAs<LiteralNode>(output.tree, separator->children.front());
        CHECK(literal && literal->value == ",");
    }
    if (replacement && !replacement->children.empty()) {
        const auto* literal = nodeAs<LiteralNode>(output.tree, replacement->children.front());
        CHECK(literal && literal->value == "$%()\\");
    }
}

void treatsQuotesBracketsAndNewlinesAsText() {
    using namespace trackknife::titleformat;
    const auto output = parse("one\r\n'two' [three]");
    CHECK(output.isValid());
    const auto* root = rootSequence(output.tree);
    CHECK(root && root->children.size() == 1U);
    if (root && !root->children.empty()) {
        const auto* literal = nodeAs<LiteralNode>(output.tree, root->children.front());
        CHECK(literal && literal->value == "one\r\n'two' [three]");
    }
}

void diagnosesMalformedSourceAndRecoversInEditorMode() {
    using namespace trackknife::titleformat;
    const auto malformed = parse("%artist $if(x,bad\\q", {.mode = ParseMode::editor});
    CHECK(!malformed.isValid());
    CHECK(!malformed.canExecute());
    CHECK(std::ranges::any_of(malformed.diagnostics, [](const Diagnostic& diagnostic) {
        return diagnostic.code == DiagnosticCode::unterminated_field;
    }));

    const auto bad_escape = parse("bad\\q", {.mode = ParseMode::editor});
    CHECK(!bad_escape.isValid());
    CHECK(bad_escape.diagnostics.size() == 1U);
    CHECK(bad_escape.diagnostics.front().code == DiagnosticCode::invalid_escape);

    const auto call = parse("$if(x,y", {.mode = ParseMode::editor});
    CHECK(std::ranges::any_of(call.diagnostics, [](const Diagnostic& diagnostic) {
        return diagnostic.code == DiagnosticCode::unterminated_call;
    }));
}

void diagnosesUnexpectedCloserAndLimits() {
    using namespace trackknife::titleformat;
    const auto closer = parse("a)", {.mode = ParseMode::editor});
    CHECK(closer.diagnostics.size() == 1U);
    CHECK(closer.diagnostics.front().code == DiagnosticCode::unexpected_closing_parenthesis);

    const auto oversized = parse("12345", {.maximum_source_bytes = 4});
    CHECK(!oversized.isValid());
    CHECK(oversized.diagnostics.front().code == DiagnosticCode::source_too_large);

    const auto nested = parse("$if(x,$if(y,a,b),c)", {.maximum_nesting_depth = 1});
    CHECK(std::ranges::any_of(nested.diagnostics, [](const Diagnostic& diagnostic) {
        return diagnostic.code == DiagnosticCode::nesting_limit_exceeded;
    }));
}

void spansAreUtf8ByteOffsets() {
    using namespace trackknife::titleformat;
    const auto output = parse("é%artist%");
    const auto* root = rootSequence(output.tree);
    CHECK(root && root->children.size() == 2U);
    if (root == nullptr || root->children.size() != 2U) {
        return;
    }
    CHECK(output.tree.node(root->children.at(0)).span == (SourceSpan{0, 2}));
    CHECK(output.tree.node(root->children.at(1)).span == (SourceSpan{2, 10}));
}

[[nodiscard]] bool
treeReferencesAndSpansAreValid(const trackknife::titleformat::ParseOutput& output) {
    using namespace trackknife::titleformat;
    if (output.tree.root() >= output.tree.nodeCount()) {
        return false;
    }
    for (std::size_t index = 0; index < output.tree.nodeCount(); ++index) {
        const auto& node = output.tree.node(static_cast<NodeId>(index));
        if (node.span.begin > node.span.end || node.span.end > output.tree.source().size()) {
            return false;
        }
        const bool references_valid = std::visit(
            [&output](const auto& data) {
                using Data = std::decay_t<decltype(data)>;
                if constexpr (std::is_same_v<Data, SequenceNode>) {
                    return std::ranges::all_of(data.children, [&output](const NodeId child) {
                        return child < output.tree.nodeCount();
                    });
                } else if constexpr (std::is_same_v<Data, CallNode>) {
                    return std::ranges::all_of(data.arguments, [&output](const NodeId argument) {
                        return argument < output.tree.nodeCount();
                    });
                } else {
                    return true;
                }
            },
            node.data);
        if (!references_valid) {
            return false;
        }
    }
    return std::ranges::all_of(output.diagnostics, [&output](const Diagnostic& diagnostic) {
        return diagnostic.span.begin <= diagnostic.span.end &&
               diagnostic.span.end <= output.tree.source().size();
    });
}

void preservesArenaInvariantsForArbitraryBytes() {
    constexpr std::array alphabet{'a',
                                  'Z',
                                  '0',
                                  ' ',
                                  '%',
                                  '$',
                                  '(',
                                  ')',
                                  ',',
                                  '\\',
                                  '[',
                                  ']',
                                  '\'',
                                  '\r',
                                  '\n',
                                  '\0',
                                  static_cast<char>(0xC3),
                                  static_cast<char>(0xA9)};
    std::uint32_t state = 0x51A7F00DU;
    for (std::size_t sample = 0; sample < 5'000U; ++sample) {
        state = state * 1'664'525U + 1'013'904'223U;
        const auto length = static_cast<std::size_t>(state % 192U);
        std::string source;
        source.reserve(length);
        for (std::size_t index = 0; index < length; ++index) {
            state = state * 1'664'525U + 1'013'904'223U;
            source.push_back(alphabet.at(state % alphabet.size()));
        }
        const auto output = trackknife::titleformat::parse(
            std::move(source),
            {.mode = trackknife::titleformat::ParseMode::editor, .maximum_nesting_depth = 32U});
        CHECK(treeReferencesAndSpansAreValid(output));
        if (failures != 0) {
            return;
        }
    }
}

} // namespace

int main() {
    try {
        parsesNestedSourcePreservingly();
        escapedSyntaxStaysLiteral();
        treatsQuotesBracketsAndNewlinesAsText();
        diagnosesMalformedSourceAndRecoversInEditorMode();
        diagnosesUnexpectedCloserAndLimits();
        spansAreUtf8ByteOffsets();
        preservesArenaInvariantsForArbitraryBytes();
    } catch (const std::exception& exception) {
        std::cerr << "unexpected parser-test exception: " << exception.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
