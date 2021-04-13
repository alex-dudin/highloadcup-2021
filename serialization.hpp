#pragma once

#include <fmt/format.h>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/writer.h>

#include <stdexcept>
#include <string>

#include "types.hpp"

class JsonError final : public std::invalid_argument
{
public:
    explicit JsonError(const std::string& message) : std::invalid_argument(message) {}
    explicit JsonError(const char* message) : std::invalid_argument(message) {}
};

class StdStringWrapper
{
public:
    using Ch = char;
    explicit StdStringWrapper(std::string& s) : s(s) {}

    void Put(char ch) { s.push_back(ch); }
    void Flush() {}
private:
    std::string& s;
};

template <class Writer>
void write_error(Writer& writer, int error_code, const char* message)
{
    writer.StartObject();

    writer.Key("code");
    writer.Int(error_code);

    writer.Key("message");
    writer.String(message);

    writer.EndObject();
}

template <class Writer>
void write_value(Writer& writer, std::nullptr_t)
{
}

template <class Writer>
void write_value(Writer& writer, const std::string& s)
{
    writer.String(s.data(), s.size());
}

template <class Writer>
void write_value(Writer& writer, const Wallet& wallet)
{
    writer.StartArray();

    for (const auto coin : wallet)
        writer.Uint(coin);

    writer.EndArray();
}

template <class Writer>
void write_value(Writer& writer, const Balance& balance)
{
    writer.StartObject();

    writer.Key("balance");
    writer.Uint(balance.balance);

    writer.Key("wallet");
    write_value(writer, balance.wallet);

    writer.EndObject();
}

template <class Writer>
void write_value(Writer& writer, const License& license)
{
    writer.StartObject();

    writer.Key("id");
    writer.Int(license.id);

    writer.Key("digAllowed");
    writer.Uint(license.dig_allowed);

    writer.Key("digUsed");
    writer.Uint(license.dig_used);

    writer.EndObject();
}

template <class Writer>
void write_value(Writer& writer, const LicenseList& licenses)
{
    writer.StartArray();

    for (const auto& license : licenses)
        write_value(writer, license);

    writer.EndArray();
}

template <class Writer>
void write_value(Writer& writer, const Area& area)
{
    writer.StartObject();

    writer.Key("posX");
    writer.Int(area.pos_x);

    writer.Key("posY");
    writer.Int(area.pos_y);

    writer.Key("sizeX");
    writer.Int(area.size_x);

    writer.Key("sizeY");
    writer.Int(area.size_y);

    writer.EndObject();
}

template <class Writer>
void write_value(Writer& writer, const Report& report)
{
    writer.StartObject();

    writer.Key("area");
    write_value(writer, report.area);

    writer.Key("amount");
    writer.Uint(report.amount);

    writer.EndObject();
}

template <class Writer>
void write_value(Writer& writer, const TreasureList& treasures)
{
    writer.StartArray();

    for (const auto& treasure : treasures)
        writer.String(treasure.data(), treasure.size());

    writer.EndArray();
}

template <class Writer>
void write_value(Writer& writer, const Dig& dig)
{
    writer.StartObject();

    writer.Key("licenseID");
    writer.Int(dig.license_id);

    writer.Key("posX");
    writer.Int(dig.pos_x);

    writer.Key("posY");
    writer.Int(dig.pos_y);

    writer.Key("depth");
    writer.Int(dig.depth);

    writer.EndObject();
}

inline int read_int(const rapidjson::Value& value, const char* value_name)
{
    if (!value.IsUint())
        throw JsonError(fmt::format("`{}` has invalid type, integer expected", value_name));

    return value.GetUint();
}

inline unsigned read_uint32(const rapidjson::Value& value, const char* value_name)
{
    if (!value.IsUint())
        throw JsonError(fmt::format("`{}` has invalid type, uint32 expected", value_name));

    return value.GetUint();
}

inline std::string read_string(const rapidjson::Value& value, const char* value_name)
{
    if (!value.IsString())
        throw JsonError(fmt::format("`{}` has invalid type, string expected", value_name));

    return std::string(value.GetString(), value.GetStringLength());
}

inline void verify_is_array(const rapidjson::Value& value, const char* value_name)
{
    if (!value.IsArray())
        throw JsonError(fmt::format("`{}` has invalid type, array expected", value_name));
}

inline void verify_is_object(const rapidjson::Value& value, const char* value_name)
{
    if (!value.IsObject())
        throw JsonError(fmt::format("`{}` has invalid type, object expected", value_name));
}

template <typename Value>
Value read_value(const rapidjson::Value& value);

template <>
inline Wallet read_value<Wallet>(const rapidjson::Value& value)
{
    verify_is_array(value, "wallet");

    Wallet wallet;
    wallet.reserve(value.Size());

    for (rapidjson::SizeType i = 0; i < value.Size(); ++i)
        wallet.push_back(read_uint32(value[i], "coin"));

    return wallet;
}

template <>
inline Balance read_value<Balance>(const rapidjson::Value& value)
{
    verify_is_object(value, "balance");

    Balance balance;

    const auto none = value.MemberEnd();
    const auto balance_value = value.FindMember("balance");
    const auto wallet = value.FindMember("wallet");

    if (balance_value == none)
        throw JsonError("`balance:balance` property is required");
    if (wallet == none)
        throw JsonError("`balance:wallet` property is required");

    balance.balance = read_uint32(balance_value->value, "balance:balance");
    balance.wallet = read_value<Wallet>(wallet->value);
    return balance;
}

template <>
inline Area read_value<Area>(const rapidjson::Value& value)
{
    verify_is_object(value, "area");

    const auto none = value.MemberEnd();
    const auto pos_x = value.FindMember("posX");
    const auto pos_y = value.FindMember("posY");
    const auto size_x = value.FindMember("sizeX");
    const auto size_y = value.FindMember("sizeY");

    if (pos_x == none)
        throw JsonError("`area:posX` property is required");
    if (pos_y == none)
        throw JsonError("`area:posY` property is required");
    if (size_x == none)
        throw JsonError("`area:sizeX` property is required");
    if (size_y == none)
        throw JsonError("`area:sizeY` property is required");

    Area area;
    area.pos_x = read_int(pos_x->value, "area:posX");
    area.pos_y = read_int(pos_y->value, "area:posY");
    area.size_x = read_int(size_x->value, "area:sizeX");
    area.size_y = read_int(size_y->value, "area:sizeY");
    return area;
}

template <>
inline Dig read_value<Dig>(const rapidjson::Value& value)
{
    verify_is_object(value, "dig");

    const auto none = value.MemberEnd();
    const auto license_id = value.FindMember("licenseID");
    const auto pos_x = value.FindMember("posX");
    const auto pos_y = value.FindMember("posY");
    const auto depth = value.FindMember("depth");

    if (license_id == none)
        throw JsonError("`dig:licenseID` property is required");
    if (pos_x == none)
        throw JsonError("`dig:posX` property is required");
    if (pos_y == none)
        throw JsonError("`dig:posX` property is required");
    if (depth == none)
        throw JsonError("`dig:depth` property is required");

    Dig dig;
    dig.license_id = read_int(license_id->value, "dig:licenseID");
    dig.pos_x = read_int(pos_x->value, "dig:posX");
    dig.pos_y = read_int(pos_y->value, "dig:posY");
    dig.depth = read_int(depth->value, "dig:depth");
    return dig;
}

template <>
inline License read_value<License>(const rapidjson::Value& value)
{
    verify_is_object(value, "license");

    const auto none = value.MemberEnd();
    const auto id = value.FindMember("id");
    const auto dig_allowed = value.FindMember("digAllowed");
    const auto dig_used = value.FindMember("digUsed");

    if (id == none)
        throw JsonError("`license:id` property is required");
    if (dig_allowed == none)
        throw JsonError("`license:digAllowed` property is required");
    if (dig_used == none)
        throw JsonError("`license:digUsed` property is required");

    License license;
    license.id = read_int(id->value, "license:id");
    license.dig_allowed = read_uint32(dig_allowed->value, "license:digAllowed");
    license.dig_used = read_uint32(dig_used->value, "license:digUsed");
    return license;
}

template <>
inline LicenseList read_value<LicenseList>(const rapidjson::Value& value)
{
    verify_is_array(value, "licenseList");

    LicenseList licenses;
    licenses.reserve(value.Size());

    for (rapidjson::SizeType i = 0; i < value.Size(); ++i)
        licenses.push_back(read_value<License>(value[i]));

    return licenses;
}

template <>
inline TreasureList read_value<TreasureList>(const rapidjson::Value& value)
{
    verify_is_array(value, "treasureList");

    TreasureList treasures;
    treasures.reserve(value.Size());

    for (rapidjson::SizeType i = 0; i < value.Size(); ++i)
        treasures.push_back(read_string(value[i], "treasure"));

    return treasures;
}

template <>
inline Report read_value<Report>(const rapidjson::Value& value)
{
    verify_is_object(value, "report");

    const auto none = value.MemberEnd();
    const auto area = value.FindMember("area");
    const auto amount = value.FindMember("amount");

    if (area == none)
        throw JsonError("`report:area` property is required");
    if (amount == none)
        throw JsonError("`report:amount` property is required");

    Report report;
    report.area = read_value<Area>(area->value);
    report.amount = read_uint32(amount->value, "report:amount");
    return report;
}

inline void parse_json(const std::string& json, rapidjson::Document& document)
{
    document.Parse(json.data(), json.size());
    if (document.HasParseError())
        throw JsonError(rapidjson::GetParseError_En(document.GetParseError()));
}
