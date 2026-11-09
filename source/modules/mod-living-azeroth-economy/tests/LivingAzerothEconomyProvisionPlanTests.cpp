#include "../src/LivingAzerothEconomyProvisionPlan.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{
using namespace LivingAzeroth::Economy;

[[noreturn]] void Fail(std::string_view message)
{
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void Require(bool condition, std::string_view message)
{
    if (!condition)
        Fail(message);
}
}

int main()
{
    ProvisionPlan empty = BuildProvisionPlan({});
    Require(empty.ok, "empty state must plan successfully");
    Require(empty.createAccount, "empty state must create one account");
    Require(empty.createCharacter, "empty state must create one character");

    ProvisionObservation accountOnly;
    accountOnly.accountExists = true;
    accountOnly.accountId = 58;
    ProvisionPlan accountOnlyPlan = BuildProvisionPlan(accountOnly);
    Require(accountOnlyPlan.ok, "account-only state must plan successfully");
    Require(!accountOnlyPlan.createAccount, "account-only state must not recreate the account");
    Require(accountOnlyPlan.createCharacter, "account-only state must create the character");

    ProvisionObservation complete;
    complete.accountExists = true;
    complete.accountId = 58;
    complete.accountCharacterCount = 1;
    complete.characterExists = true;
    complete.characterGuid = 900001;
    complete.characterAccountId = 58;
    complete.characterRace = ProvisionRace;
    complete.characterClass = ProvisionClass;
    complete.characterGender = ProvisionGender;
    ProvisionPlan second = BuildProvisionPlan(complete);
    Require(second.ok, "complete state must plan successfully");
    Require(!second.createAccount, "complete state must not recreate the account");
    Require(!second.createCharacter, "complete state must not recreate the character");
    Require(second.accountId == 58, "complete state must resolve the account");
    Require(second.characterGuid == 900001, "complete state must resolve the character");

    auto nameCollision = ProvisionObservation{};
    nameCollision.characterExists = true;
    nameCollision.characterGuid = 700;
    nameCollision.characterAccountId = 99;
    ProvisionPlan nameCollisionPlan = BuildProvisionPlan(nameCollision);
    Require(!nameCollisionPlan.ok, "character collision without account must fail closed");

    auto ownerMismatch = complete;
    ownerMismatch.characterAccountId = 99;
    ProvisionPlan ownerMismatchPlan = BuildProvisionPlan(ownerMismatch);
    Require(!ownerMismatchPlan.ok, "character ownership mismatch must fail closed");

    auto multipleCharacters = complete;
    multipleCharacters.accountCharacterCount = 2;
    ProvisionPlan multipleCharactersPlan = BuildProvisionPlan(multipleCharacters);
    Require(!multipleCharactersPlan.ok, "multiple characters must fail closed");

    auto elevatedAccount = complete;
    elevatedAccount.accountSecurity = 3;
    ProvisionPlan elevatedAccountPlan = BuildProvisionPlan(elevatedAccount);
    Require(!elevatedAccountPlan.ok, "elevated account must fail closed");

    auto identityMismatch = complete;
    identityMismatch.characterRace = 2;
    ProvisionPlan identityMismatchPlan = BuildProvisionPlan(identityMismatch);
    Require(!identityMismatchPlan.ok, "identity shape mismatch must fail closed");

    std::cout
        << "{\"result\":\"PASS\",\"empty_account_creates\":1,\"empty_character_creates\":1,"
        << "\"complete_mutations\":0,\"account_only_character_creates\":1,"
        << "\"collision_cases_fail_closed\":4}\n";
    return 0;
}
