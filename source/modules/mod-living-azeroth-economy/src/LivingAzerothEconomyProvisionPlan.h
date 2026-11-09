/*
 * Living Azeroth AI4 deterministic seller identity provisioning planner.
 */

#ifndef LIVING_AZEROTH_ECONOMY_PROVISION_PLAN_H
#define LIVING_AZEROTH_ECONOMY_PROVISION_PLAN_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace LivingAzeroth::Economy
{
inline constexpr std::string_view ProvisionAccountName = "EXAMPLEECONOMY";
inline constexpr std::string_view ProvisionCharacterName = "ExampleSeller";
inline constexpr std::uint8_t ProvisionRace = 1;
inline constexpr std::uint8_t ProvisionClass = 1;
inline constexpr std::uint8_t ProvisionGender = 0;

struct ProvisionObservation
{
    bool accountExists = false;
    std::uint32_t accountId = 0;
    std::uint32_t accountSecurity = 0;
    std::size_t accountCharacterCount = 0;
    bool characterExists = false;
    std::uint32_t characterGuid = 0;
    std::uint32_t characterAccountId = 0;
    std::uint8_t characterRace = 0;
    std::uint8_t characterClass = 0;
    std::uint8_t characterGender = 0;
};

struct ProvisionPlan
{
    bool ok = false;
    std::string error;
    bool createAccount = false;
    bool createCharacter = false;
    std::uint32_t accountId = 0;
    std::uint32_t characterGuid = 0;
};

inline ProvisionPlan BuildProvisionPlan(ProvisionObservation const& observed)
{
    ProvisionPlan plan;

    if (!observed.accountExists)
    {
        if (observed.accountId != 0 || observed.accountSecurity != 0 ||
            observed.accountCharacterCount != 0)
        {
            plan.error = "missing_account_has_inconsistent_metadata";
            return plan;
        }

        if (observed.characterExists || observed.characterGuid != 0 ||
            observed.characterAccountId != 0)
        {
            plan.error = "seller_character_name_collision_without_account";
            return plan;
        }

        plan.ok = true;
        plan.createAccount = true;
        plan.createCharacter = true;
        return plan;
    }

    if (observed.accountId == 0)
    {
        plan.error = "seller_account_id_missing";
        return plan;
    }

    if (observed.accountSecurity != 0)
    {
        plan.error = "seller_account_must_be_player_security";
        return plan;
    }

    if (observed.accountCharacterCount > 1)
    {
        plan.error = "seller_account_has_multiple_characters";
        return plan;
    }

    plan.accountId = observed.accountId;
    if (!observed.characterExists)
    {
        if (observed.characterGuid != 0 || observed.characterAccountId != 0)
        {
            plan.error = "missing_character_has_inconsistent_metadata";
            return plan;
        }

        if (observed.accountCharacterCount != 0)
        {
            plan.error = "seller_account_has_unexpected_character";
            return plan;
        }

        plan.ok = true;
        plan.createCharacter = true;
        return plan;
    }

    if (observed.characterGuid == 0)
    {
        plan.error = "seller_character_guid_missing";
        return plan;
    }

    if (observed.characterAccountId != observed.accountId)
    {
        plan.error = "seller_character_account_mismatch";
        return plan;
    }

    if (observed.accountCharacterCount != 1)
    {
        plan.error = "seller_account_character_count_must_equal_1";
        return plan;
    }

    if (observed.characterRace != ProvisionRace ||
        observed.characterClass != ProvisionClass ||
        observed.characterGender != ProvisionGender)
    {
        plan.error = "seller_character_identity_shape_mismatch";
        return plan;
    }

    plan.ok = true;
    plan.accountId = observed.accountId;
    plan.characterGuid = observed.characterGuid;
    return plan;
}
}

#endif
