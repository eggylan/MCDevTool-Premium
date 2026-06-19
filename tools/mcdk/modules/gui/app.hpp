#pragma once

#include <nlohmann/json.hpp>

namespace mcdk::gui {

    int runGui(int argc, char* argv[], const nlohmann::json& config);

} // namespace mcdk::gui
