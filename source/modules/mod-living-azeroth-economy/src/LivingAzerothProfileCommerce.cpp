#include "LivingAzerothProfileCommercePlan.h"

#include "Config.h"
#include "Log.h"
#include "ScriptMgr.h"
#include "WorldScript.h"

#include <algorithm>
#include <cstdint>

namespace
{
using LivingAzeroth::Economy::ProfileCommerce::Policy;

class LivingAzerothProfileCommerceScript final : public WorldScript
{
public:
    LivingAzerothProfileCommerceScript()
        : WorldScript("LivingAzerothProfileCommerceScript",
            { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED,
              WORLDHOOK_ON_STARTUP })
    {
    }

    void OnAfterConfigLoad(bool reload) override
    {
        if (reload)
            LoadConfig();
    }

    void OnBeforeWorldInitialized() override
    {
        LoadConfig();
    }

    void OnStartup() override
    {
        if (!_policy.Enabled)
        {
            LOG_INFO("living_azeroth.economy",
                "Living Azeroth profile commerce disabled (safe default): masterEnabled={}.",
                _masterEnabled);
            return;
        }

        LOG_INFO("living_azeroth.economy",
            "Living Azeroth profile commerce policy loaded: masterEnabled={}, observeOnly={}, seller={}, buyer={}, "
            "allowHumanAuctions={}, priceBand={}-{}%, buyerCeiling={}%, listingCap={}, "
            "dailyPurchaseCap={}, dailySpendCap={}, minimumReserve={}; actuatorRegistered=true, "
            "mutationRequested={}.",
            _masterEnabled, _policy.ObserveOnly, _policy.SellerEnabled, _policy.BuyerEnabled,
            _policy.AllowHumanAuctions, _policy.MinimumUnitPricePercent,
            _policy.MaximumUnitPricePercent, _policy.BuyerCeilingPercent,
            _policy.MaxListingsPerProfile, _policy.MaxPurchasesPerProfilePerDay,
            _policy.MaxSpendPerProfilePerDay, _policy.MinimumMoneyReserve,
            !_policy.ObserveOnly && (_policy.SellerEnabled || _policy.BuyerEnabled));
    }

private:
    void LoadConfig()
    {
        _masterEnabled = sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.Enable", false);
        bool const profileEnabled = sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.ProfileCommerce.Enable", false);
        _policy.Enabled = _masterEnabled && profileEnabled;
        _policy.ObserveOnly = sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.ProfileCommerce.ObserveOnly", true);
        _policy.SellerEnabled = sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.ProfileCommerce.Seller.Enable", false);
        _policy.BuyerEnabled = sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.ProfileCommerce.Buyer.Enable", false);
        _policy.AllowHumanAuctions = sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.ProfileCommerce.Buyer.AllowHumanAuctions", false);
        _policy.MinimumUnitPricePercent = std::clamp(
            sConfigMgr->GetOption<std::uint32_t>(
                "LivingAzeroth.Economy.ProfileCommerce.Price.MinimumPercent", 80),
            std::uint32_t(50), std::uint32_t(100));
        _policy.MaximumUnitPricePercent = std::clamp(
            sConfigMgr->GetOption<std::uint32_t>(
                "LivingAzeroth.Economy.ProfileCommerce.Price.MaximumPercent", 120),
            std::uint32_t(100), std::uint32_t(200));
        _policy.BuyerCeilingPercent = std::clamp(
            sConfigMgr->GetOption<std::uint32_t>(
                "LivingAzeroth.Economy.ProfileCommerce.Buyer.CeilingPercent", 110),
            std::uint32_t(50), std::uint32_t(150));
        _policy.MaxListingsPerProfile = std::clamp(
            sConfigMgr->GetOption<std::uint32_t>(
                "LivingAzeroth.Economy.ProfileCommerce.Seller.MaxListingsPerProfile", 3),
            std::uint32_t(1), std::uint32_t(10));
        _policy.MaxSellerActionsPerCycle = std::clamp(
            sConfigMgr->GetOption<std::uint32_t>(
                "LivingAzeroth.Economy.ProfileCommerce.Seller.MaxActionsPerCycle", 2),
            std::uint32_t(1), std::uint32_t(10));
        _policy.MaxBuyerActionsPerCycle = std::clamp(
            sConfigMgr->GetOption<std::uint32_t>(
                "LivingAzeroth.Economy.ProfileCommerce.Buyer.MaxActionsPerCycle", 1),
            std::uint32_t(1), std::uint32_t(5));
        _policy.MaxPurchasesPerProfilePerDay = std::clamp(
            sConfigMgr->GetOption<std::uint32_t>(
                "LivingAzeroth.Economy.ProfileCommerce.Buyer.MaxPurchasesPerProfilePerDay", 2),
            std::uint32_t(1), std::uint32_t(10));
        _policy.MaxSpendPerProfilePerDay = sConfigMgr->GetOption<std::uint64_t>(
            "LivingAzeroth.Economy.ProfileCommerce.Buyer.MaxSpendCopperPerProfilePerDay", 20000);
        _policy.MinimumMoneyReserve = sConfigMgr->GetOption<std::uint64_t>(
            "LivingAzeroth.Economy.ProfileCommerce.Buyer.MinimumReserveCopper", 1000);
    }

    Policy _policy;
    bool _masterEnabled = false;
};
}

void AddLivingAzerothProfileCommerceScripts()
{
    new LivingAzerothProfileCommerceScript();
}
