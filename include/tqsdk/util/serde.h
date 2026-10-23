#pragma once

#include "tqsdk/pch.h"

#define DEFINE_ENUM_JSON_SERIALIZE(EnumType)                                                       \
    inline void to_json(::nlohmann::json& j, const EnumType& e) {                                  \
        auto name = ::magic_enum::enum_name(e);                                                    \
        if (name.empty()) {                                                                        \
            auto raw = static_cast<std::underlying_type_t<EnumType>>(e);                           \
            throw std::invalid_argument(std::string("Invalid " #EnumType " value: ") +             \
                                        std::to_string(raw));                                      \
        }                                                                                          \
        j = std::string(name);                                                                     \
    }                                                                                              \
    inline void from_json(const ::nlohmann::json& j, EnumType& e) {                                \
        if (!j.is_string()) {                                                                      \
            throw std::invalid_argument(std::string("Expected string for " #EnumType ", got: ") +  \
                                        j.dump());                                                 \
        }                                                                                          \
        auto opt = ::magic_enum::enum_cast<EnumType>(j.get_ref<const std::string&>());             \
        if (!opt) {                                                                                \
            throw std::invalid_argument(std::string("Invalid " #EnumType " value: ") + j.dump());  \
        }                                                                                          \
        e = *opt;                                                                                  \
    }

#define DEFINE_ENUM_JSON_SERIALIZE_FALLBACK(EnumType, Fallback)                                    \
    inline void to_json(::nlohmann::json& j, const EnumType& e) {                                  \
        auto name = ::magic_enum::enum_name(e);                                                    \
        j         = name.empty() ? std::string(#Fallback) : std::string(name);                     \
    }                                                                                              \
    inline void from_json(const ::nlohmann::json& j, EnumType& e) {                                \
        auto opt = j.is_string()                                                                   \
                       ? ::magic_enum::enum_cast<EnumType>(j.get_ref<const std::string&>())        \
                       : std::nullopt;                                                             \
        e        = opt.value_or(EnumType::Fallback);                                               \
    }
