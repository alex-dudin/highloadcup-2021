#pragma once

#include <string>
#include <vector>

/// Map area.
struct Area
{
    int pos_x = 0;
    int pos_y = 0;
    int size_x = 1;
    int size_y = 1;

    bool contains(Area other) const
    {
        return other.pos_x >= pos_x
            && other.pos_y >= pos_y
            && other.pos_x + other.size_x <= pos_x + size_x
            && other.pos_y + other.size_y <= pos_y + size_y;
    }
};

inline bool operator==(const Area& lhs, const Area& rhs)
{
    return lhs.pos_x == rhs.pos_x
        && lhs.pos_y == rhs.pos_y
        && lhs.size_x == rhs.size_x
        && lhs.size_y == rhs.size_y;
}

inline bool operator!=(const Area& lhs, const Area& rhs)
{
    return !(lhs == rhs);
}

/// Non-negative amount of treasures/etc.
using Amount = unsigned;

/// License ID.
using LicenseId = int;

/// License for digging.
struct License
{
    LicenseId id = 0;
    Amount dig_allowed = 0;
    Amount dig_used = 0;

    bool expired() const noexcept
    {
        return dig_allowed == dig_used;
    }

    bool use() noexcept
    {
        if (expired())
            return false;

        dig_used++;
        return true;
    }
};

/// List of issued licenses.
using LicenseList = std::vector<License>;

/// Treasure ID.
using Treasure = std::string;

/// List of treasures.
using TreasureList = std::vector<Treasure>;

/// Treasure position.
struct TreasurePosition
{
    int x = 0;
    int y = 0;
    int depth = 0;
};

struct TreasureInfo
{
    Treasure treasure;
    int depth = 0;
};

/// Coin ID.
using Coin = unsigned;

/// Wallet with some coins.
using Wallet = std::vector<Coin>;

/// Current balance and wallet with up to 1000 coins.
struct Balance
{
    Amount balance = 0;
    Wallet wallet;

    /// Wallet max capacity;
    static constexpr size_t wallet_max_capacity = 1000;
};

/// Report about found treasures.
struct Report
{
    Area area;
    Amount amount = 0;
};

using ReportList = std::vector<Report>;

struct Dig
{
    LicenseId license_id = 0;
    int pos_x = 0;
    int pos_y = 0;
    int depth = 0;
};

struct TreasureCostRange
{
    Amount min_cost = 0;
    Amount max_cost = 0;

    bool contains(Amount amount) const
    {
        return amount >= min_cost && amount <= max_cost;
    }

    Amount delta() const
    {
        return max_cost - min_cost;
    }
};
