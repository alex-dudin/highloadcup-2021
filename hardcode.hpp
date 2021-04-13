#pragma once

#include <bitset>
#include "types.hpp"

static constexpr int MAP_SIZE = 3500;
static constexpr Area MAP_AREA{ 0, 0, MAP_SIZE, MAP_SIZE };
static constexpr int MAX_DEPTH = 10;
static constexpr unsigned MAX_LICENSE_COUNT = 10;
static constexpr unsigned TREASURE_COUNT = 490'000;
static constexpr unsigned MAX_TREASURE_COUNT = 600'000;
static constexpr unsigned MIN_TREASURE_COUNT = 10'000;
static constexpr unsigned LIFETIME = 600;
static constexpr unsigned FIRST_API_CALL_TIMEOUT = 60;
static constexpr unsigned GAME_TYPE_COUNT = 5;

static constexpr int64_t ROUND2_SEED = 845326129;
static constexpr int64_t ROUND3_SEED = 1102183371;

static constexpr uint64_t MEMORY_LIMIT = 2'147'483'647;
static constexpr int CPU_COUNT = 4;

extern const TreasureCostRange TREASURE_COST_RANGES[GAME_TYPE_COUNT][MAX_DEPTH];
extern const int DROP_LEVELS[GAME_TYPE_COUNT];
extern const int OPTIMAL_LEVELS[GAME_TYPE_COUNT];
extern const std::bitset<MAX_DEPTH> MAIN_LEVELS[GAME_TYPE_COUNT];
