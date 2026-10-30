// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// How a parameter name is matched, shared by local and remote statements.
#pragma once

#include <string>
#include <string_view>

namespace sql::internal {

/// `name` spelled with its prefix (":id") matches exactly; spelled without
/// one ("id") it matches :id, @id or $id.
inline bool parameterMatches(const std::string& spelled, std::string_view name) noexcept
{
    if (spelled == name)
        return true;
    if (name.empty() || name[0] == ':' || name[0] == '@' || name[0] == '$' || name[0] == '?')
        return false;
    return spelled.size() == name.size() + 1 && spelled[0] != '?' &&
           std::string_view(spelled).substr(1) == name;
}

}  // namespace sql::internal
