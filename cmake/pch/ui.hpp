// SPDX-License-Identifier: GPL-3.0-only

// Precompiled for the UI's targets and their tests: headers that do not
// change while Melody is worked on, parsed once instead of in every file.
// Each Qt module where the target has it.
#include <QtCore>
#if __has_include(<QtConcurrent>)
#include <QtConcurrent>
#endif
#if __has_include(<QtGui>)
#include <QtGui>
#endif
#if __has_include(<QtNetwork>)
#include <QtNetwork>
#endif
#if __has_include(<QtWidgets>)
#include <QtWidgets>
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>
