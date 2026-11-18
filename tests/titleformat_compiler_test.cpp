// SPDX-License-Identifier: GPL-3.0-only

#include "test_checks.hpp"
#include "trackknife/titleformat/compiler.hpp"
#include "trackknife/titleformat/function_registry.hpp"
#include "trackknife/titleformat/value.hpp"

#include <array>
#include <iostream>
#include <string_view>
#include <variant>

namespace {

using checks::failures;

void resolvesFunctionsDependenciesAndDialect() {
    using namespace trackknife::titleformat;
    const std::string source =
        "$IF(%Artist%,$upper(%TITLE%),%album%)%ARTIST%$join(genre,;)$info(codec)";
    const auto output = compile(source);
    CHECK(output.isValid());
    CHECK(output.program.has_value());
    if (!output.program) {
        return;
    }
    CHECK(output.program->syntax().source() == source);
    CHECK(output.program->dialectVersion().dialect == "tkfmt");
    CHECK(output.program->dialectVersion().dialect_version == 1U);
    CHECK(output.program->dialectVersion().compiler_schema == 2U);
    const auto cache_key = output.program->cacheKey();
    CHECK(cache_key.dialect == output.program->dialectVersion());
    CHECK(cache_key.context == FormatContextKind::track_display);
    CHECK(cache_key.source == source);
    CHECK(cache_key.function_registry_revision == 1U);
    const std::vector<std::string> expected_fields{"artist", "title", "album", "genre"};
    CHECK(output.program->fieldDependencies() == expected_fields);
    const std::vector<std::string> expected_information{"codec"};
    CHECK(output.program->technicalDependencies() == expected_information);

    std::size_t resolved_calls = 0;
    for (std::size_t index = 0; index < output.program->syntax().nodeCount(); ++index) {
        const auto id = static_cast<NodeId>(index);
        if (std::holds_alternative<CallNode>(output.program->syntax().node(id).data)) {
            CHECK(output.program->functionAt(id).has_value());
            ++resolved_calls;
        }
    }
    CHECK(resolved_calls == 4U);

    CompileOptions queue_options;
    queue_options.context = FormatContextKind::queue;
    const auto queue = compile(source, queue_options);
    CHECK(queue.program.has_value());
    CHECK(queue.program && queue.program->cacheKey() != cache_key);
}

void rejectsUnknownDialectFunctionsArityAndSyntax() {
    using namespace trackknife::titleformat;
    auto options = CompileOptions{};
    options.dialect.dialect = "fb2k";
    const auto dialect = compile("%artist%", options);
    CHECK(!dialect.isValid());
    CHECK(dialect.diagnostics.front().code == CompileDiagnosticCode::unsupported_dialect);

    const auto unknown = compile("$does_not_exist()");
    CHECK(!unknown.isValid());
    CHECK(unknown.diagnostics.front().code == CompileDiagnosticCode::unknown_function);

    const auto arity = compile("$if(only_one)");
    CHECK(!arity.isValid());
    CHECK(arity.diagnostics.front().code == CompileDiagnosticCode::wrong_argument_count);

    const auto replacement_pairs = compile("$replace(text,a,b,c)");
    CHECK(!replacement_pairs.isValid());
    CHECK(replacement_pairs.diagnostics.front().code ==
          CompileDiagnosticCode::wrong_argument_count);

    const auto malformed = compile("%artist");
    CHECK(!malformed.isValid());
    CHECK(!malformed.parse_diagnostics.empty());

    const auto dynamic_metadata = compile("$get(%field_name%)");
    CHECK(!dynamic_metadata.isValid());
    CHECK(dynamic_metadata.diagnostics.front().code == CompileDiagnosticCode::invalid_argument);
}

void validatesTreeOnlyExpansionDependencies() {
    using namespace trackknife::titleformat;
    CompileOptions tree_options;
    tree_options.context = FormatContextKind::tree_level;
    const auto tree = compile("$each(Genre) / $each(mood) / $each(genre)", tree_options);
    CHECK(tree.isValid());
    CHECK(tree.program.has_value());
    if (tree.program) {
        const std::vector<std::string> expected{"genre", "mood"};
        CHECK(tree.program->expansionDependencies() == expected);
        CHECK(tree.program->hasExpansions());
    }

    const auto display = compile("$each(genre)");
    CHECK(!display.isValid());
    CHECK(display.diagnostics.front().code == CompileDiagnosticCode::unavailable_in_context);

    const auto dynamic = compile("$each(%field_name%)", tree_options);
    CHECK(!dynamic.isValid());
    CHECK(dynamic.diagnostics.front().code == CompileDiagnosticCode::invalid_argument);
}

void catalogsOnlyTkfmtOneBuiltins() {
    using trackknife::titleformat::findFunction;
    constexpr std::array names{
        "if",     "if2",     "and",   "or",    "not",      "eq",   "ne",       "eqi",   "gt",
        "gte",    "lt",      "lte",   "add",   "sub",      "mul",  "div",      "mod",   "min",
        "max",    "num",     "lower", "upper", "trim",     "len",  "left",     "right", "longest",
        "repeat", "replace", "pad",   "get",   "getmulti", "join", "lenmulti", "info",  "each"};
    for (const auto* name : names) {
        CHECK(findFunction(name) != nullptr);
    }
    CHECK(findFunction("IF") != nullptr);
    CHECK(findFunction("set") == nullptr);
    CHECK(findFunction("put") == nullptr);
    CHECK(findFunction("rgb") == nullptr);
}

void valuesUseOrdinaryStringTruthiness() {
    const trackknife::titleformat::EvalValue zero{"0"};
    const trackknife::titleformat::EvalValue empty{""};
    CHECK(zero.truthy());
    CHECK(!empty.truthy());
}

} // namespace

int main() {
    resolvesFunctionsDependenciesAndDialect();
    rejectsUnknownDialectFunctionsArityAndSyntax();
    validatesTreeOnlyExpansionDependencies();
    catalogsOnlyTkfmtOneBuiltins();
    valuesUseOrdinaryStringTruthiness();
    return failures == 0 ? 0 : 1;
}
