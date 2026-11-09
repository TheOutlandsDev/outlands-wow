#include "LivingAzerothCommerceSettlementPlan.h"
#include "LivingAzerothCommerceRotationPlan.h"
#include "LivingAzerothEconomyPlan.h"
#include "LivingAzerothFullPriceIndex.h"
#include "LivingAzerothProfileCommercePlan.h"
#include "LivingAzerothAutomatedListingBridge.h"
#include "LivingAzerothReachableItemCatalog.h"

#include "AuctionHouseMgr.h"
#include "Bag.h"
#include "CharacterCache.h"
#include "Config.h"
#include "Creature.h"
#include "CryptoHash.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ItemUsageValue.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "Util.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
using LivingAzeroth::Economy::ProfileCommerce::BuyerObservation;
using LivingAzeroth::Economy::ProfileCommerce::CalculateAuctionCutForBuyout;
using LivingAzeroth::Economy::ProfileCommerce::Decision;
using LivingAzeroth::Economy::ProfileCommerce::DecisionReason;
using LivingAzeroth::Economy::ProfileCommerce::OwnerKind;
using LivingAzeroth::Economy::ProfileCommerce::PlanBuyer;
using LivingAzeroth::Economy::ProfileCommerce::PlanSeller;
using LivingAzeroth::Economy::ProfileCommerce::Policy;
using LivingAzeroth::Economy::ProfileCommerce::PlanSettlement;
using LivingAzeroth::Economy::ProfileCommerce::SaturatingMultiply;
using LivingAzeroth::Economy::ProfileCommerce::SellerObservation;
using LivingAzeroth::Economy::ProfileCommerce::SettlementActionKind;
using LivingAzeroth::Economy::ProfileCommerce::SettlementObservation;
using LivingAzeroth::Economy::ProfileCommerce::SettlementOutcome;
namespace CommerceRotation = LivingAzeroth::Economy::CommerceRotation;

constexpr std::array<AuctionHouseId, 3> AuctionHouses = {
    AuctionHouseId::Alliance, AuctionHouseId::Horde, AuctionHouseId::Neutral
};
constexpr std::uint32_t PersistentEventSeconds = 3 * DAY;
struct CommerceTarget
{
    std::uint32_t Map;
    float X;
    float Y;
    float Z;
    float O;
};
constexpr CommerceTarget AllianceCommerceTarget = { 0, -8823.0f, 659.9f, 97.47f, 0.4f };
constexpr CommerceTarget HordeCommerceTarget = { 1, 1669.0f, -4463.8f, 20.39f, 1.7f };

template <std::size_t N>
constexpr std::size_t SqlPlaceholderCount(char const (&text)[N])
{
    std::size_t count = 0;
    for (std::size_t index = 0; index + 1 < N; ++index)
    {
        if (text[index] == '{' && text[index + 1] == '}')
        {
            ++count;
            ++index;
        }
    }
    return count;
}

constexpr char DispatchLedgerSql[] =
    "INSERT IGNORE INTO la_profile_commerce_ledger "
    "(action_key,phase,action_kind,character_guid,account_id,seller_guid,house_id,"
    "target_id,item_guid,item_entry,item_count,total_price_copper,deposit_copper,"
    "cut_copper,wallet_before_copper,recorded_at_unix,evidence_sha256) VALUES "
    "('{}','dispatched','{}',{},{},{},{},{},{},{},{},{},{},{},{},{},'{}')";
constexpr char DailyUsageSql[] =
    "INSERT INTO la_profile_commerce_daily_usage "
    "(character_guid,utc_day,dispatch_count,dispatch_spend_copper) "
    "VALUES ({},{},1,{}) ON DUPLICATE KEY UPDATE "
    "dispatch_count=dispatch_count+1,"
    "dispatch_spend_copper=dispatch_spend_copper+{}";
constexpr char SettlementLedgerSql[] =
    "INSERT IGNORE INTO la_profile_commerce_ledger "
    "(action_key,phase,action_kind,character_guid,account_id,seller_guid,house_id,"
    "target_id,item_guid,item_entry,item_count,total_price_copper,deposit_copper,"
    "cut_copper,wallet_before_copper,recorded_at_unix,evidence_sha256) VALUES "
    "('{}','{}','{}',{},{},{},{},{},{},{},{},{},{},{},{},{},'{}')";
constexpr char SettlementEvidenceSql[] =
    "INSERT IGNORE INTO la_profile_commerce_settlement "
    "(action_key,outcome,auction_id,item_guid,buyer_debit_copper,seller_mail_copper,"
    "deposit_copper,cut_copper,evidence_sha256,recorded_at_unix) VALUES "
    "('{}','{}',{},{},{},{},{},{},'{}',{})";

static_assert(SqlPlaceholderCount(DispatchLedgerSql) == 16);
static_assert(SqlPlaceholderCount(DailyUsageSql) == 4);
static_assert(SqlPlaceholderCount(SettlementLedgerSql) == 17);
static_assert(SqlPlaceholderCount(SettlementEvidenceSql) == 10);
struct RuntimeConfig
{
    Policy CommercePolicy;
    bool MasterEnabled = false;
    std::uint32_t CycleMs = 60 * IN_MILLISECONDS;
    std::unordered_set<std::uint32_t> OwnedSystemSellerGuids;
    std::uint32_t LockedSystemSellerGuid = 900001;
    std::string LockedSystemSellerName = "ExampleSeller";
    std::string CatalogSha256;
    std::string HumanAuctionMutationConfirmation;
    std::string ListingEnvironmentId;
    std::uint32_t ListingRealmId = 0;
    std::string ListingPolicyVersion;
    bool RotationEnabled = false;
    std::uint32_t RotationMaximumPerFaction = 0;
    std::string RotationConfirmation;
};

std::unordered_set<std::uint32_t> ParseGuidSet(std::string const& value)
{
    std::unordered_set<std::uint32_t> result;
    std::stringstream input(value);
    std::string token;
    while (std::getline(input, token, ','))
    {
        token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char character)
        {
            return std::isspace(character) != 0;
        }), token.end());
        if (token.empty())
            continue;
        try
        {
            std::size_t consumed = 0;
            unsigned long parsed = std::stoul(token, &consumed, 10);
            if (consumed == token.size() && parsed > 0 &&
                parsed <= std::numeric_limits<std::uint32_t>::max())
                result.insert(static_cast<std::uint32_t>(parsed));
        }
        catch (...)
        {
            LOG_WARN("living_azeroth.economy",
                "Profile commerce ignored invalid owned-system seller GUID token '{}'.", token);
        }
    }
    return result;
}

char const* ReasonName(DecisionReason reason)
{
    switch (reason)
    {
        case DecisionReason::Approved: return "approved";
        case DecisionReason::FeatureDisabled: return "feature_disabled";
        case DecisionReason::ObserveOnly: return "observe_only";
        case DecisionReason::NotOwnedAi: return "not_owned_ai";
        case DecisionReason::NotAtAuctioneer: return "not_at_auctioneer";
        case DecisionReason::CatalogRejected: return "catalog_rejected";
        case DecisionReason::UnsafeItem: return "unsafe_item";
        case DecisionReason::ItemStillNeeded: return "item_still_needed";
        case DecisionReason::MissingReferencePrice: return "missing_reference_price";
        case DecisionReason::ZeroPrice: return "zero_price";
        case DecisionReason::BelowNormalPriceBand: return "below_normal_price_band";
        case DecisionReason::AboveNormalPriceBand: return "above_normal_price_band";
        case DecisionReason::ProfileListingCap: return "profile_listing_cap";
        case DecisionReason::SellerCycleCap: return "seller_cycle_cap";
        case DecisionReason::SellerDepositUnaffordable: return "seller_deposit_unaffordable";
        case DecisionReason::BootMutationCap: return "boot_mutation_cap";
        case DecisionReason::DuplicateAction: return "duplicate_action";
        case DecisionReason::InvalidAuction: return "invalid_auction";
        case DecisionReason::SelfTrade: return "self_trade";
        case DecisionReason::ProtectedHumanAuction: return "protected_human_auction";
        case DecisionReason::UnownedSeller: return "unowned_seller";
        case DecisionReason::PurchaseNotUseful: return "purchase_not_useful";
        case DecisionReason::InsufficientFunds: return "insufficient_funds";
        case DecisionReason::ReserveViolation: return "reserve_violation";
        case DecisionReason::PurchasePriceCeiling: return "purchase_price_ceiling";
        case DecisionReason::DailyPurchaseCap: return "daily_purchase_cap";
        case DecisionReason::DailySpendCap: return "daily_spend_cap";
        case DecisionReason::BuyerCycleCap: return "buyer_cycle_cap";
    }
    return "unknown";
}

class LivingAzerothProfileCommerceActuator final : public WorldScript
{
public:
    LivingAzerothProfileCommerceActuator()
        : WorldScript("LivingAzerothProfileCommerceActuator",
            { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED,
              WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE })
    {
    }

    void OnAfterConfigLoad(bool reload) override
    {
        if (reload)
            LoadConfig(true);
    }

    void OnBeforeWorldInitialized() override
    {
        LoadConfig(false);
    }

private:
    void LoadConfig(bool reload)
    {
        // Block first so a partial or invalid reload can never leave the old
        // mutation policy active. Boot counters and duplicate sets are not
        // reset by a reload.
        _runtimeBlocked = true;
        Policy& policy = _config.CommercePolicy;
        _config.MasterEnabled = sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.Enable", false);
        bool const profileEnabled = sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.ProfileCommerce.Enable", false);
        policy.Enabled = _config.MasterEnabled && profileEnabled;
        policy.ObserveOnly = sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.ProfileCommerce.ObserveOnly", true);
        policy.SellerEnabled = sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.ProfileCommerce.Seller.Enable", false);
        policy.BuyerEnabled = sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.ProfileCommerce.Buyer.Enable", false);
        policy.AllowHumanAuctions = sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.ProfileCommerce.Buyer.AllowHumanAuctions", false);
        policy.RequireCatalogApproval = sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.ProfileCommerce.RequireCatalogApproval", true);
        policy.MinimumUnitPricePercent = std::clamp(
            sConfigMgr->GetOption<std::uint32_t>(
                "LivingAzeroth.Economy.ProfileCommerce.Price.MinimumPercent", 80),
            std::uint32_t(50), std::uint32_t(100));
        policy.MaximumUnitPricePercent = std::clamp(
            sConfigMgr->GetOption<std::uint32_t>(
                "LivingAzeroth.Economy.ProfileCommerce.Price.MaximumPercent", 120),
            std::uint32_t(100), std::uint32_t(200));
        policy.BuyerCeilingPercent = std::clamp(
            sConfigMgr->GetOption<std::uint32_t>(
                "LivingAzeroth.Economy.ProfileCommerce.Buyer.CeilingPercent", 110),
            std::uint32_t(50), std::uint32_t(150));
        policy.MaxListingsPerProfile = std::clamp(
            sConfigMgr->GetOption<std::uint32_t>(
                "LivingAzeroth.Economy.ProfileCommerce.Seller.MaxListingsPerProfile", 3),
            std::uint32_t(1), std::uint32_t(10));
        policy.MaxSellerActionsPerCycle = std::clamp(
            sConfigMgr->GetOption<std::uint32_t>(
                "LivingAzeroth.Economy.ProfileCommerce.Seller.MaxActionsPerCycle", 2),
            std::uint32_t(1), std::uint32_t(10));
        policy.MaxBuyerActionsPerCycle = std::clamp(
            sConfigMgr->GetOption<std::uint32_t>(
                "LivingAzeroth.Economy.ProfileCommerce.Buyer.MaxActionsPerCycle", 1),
            std::uint32_t(1), std::uint32_t(5));
        policy.MaxPurchasesPerProfilePerDay = std::clamp(
            sConfigMgr->GetOption<std::uint32_t>(
                "LivingAzeroth.Economy.ProfileCommerce.Buyer.MaxPurchasesPerProfilePerDay", 2),
            std::uint32_t(1), std::uint32_t(10));
        policy.MaxSpendPerProfilePerDay = std::min<std::uint64_t>(
            sConfigMgr->GetOption<std::uint64_t>(
                "LivingAzeroth.Economy.ProfileCommerce.Buyer.MaxSpendCopperPerProfilePerDay", 20000),
            std::numeric_limits<std::uint32_t>::max());
        policy.MinimumMoneyReserve = sConfigMgr->GetOption<std::uint64_t>(
            "LivingAzeroth.Economy.ProfileCommerce.Buyer.MinimumReserveCopper", 1000);
        policy.MaxMutationsPerBoot = std::min<std::uint32_t>(
            sConfigMgr->GetOption<std::uint32_t>(
                "LivingAzeroth.Economy.ProfileCommerce.MaxMutationsPerBoot", 0),
            std::uint32_t(1000));
        _config.CycleMs = std::clamp(
            sConfigMgr->GetOption<std::uint32_t>(
                "LivingAzeroth.Economy.ProfileCommerce.CycleSeconds", 60),
            std::uint32_t(30), std::uint32_t(3600)) * IN_MILLISECONDS;
        _config.OwnedSystemSellerGuids = ParseGuidSet(sConfigMgr->GetOption<std::string>(
            "LivingAzeroth.Economy.ProfileCommerce.OwnedSystemSellerGuids", "900001"));
        _config.LockedSystemSellerGuid = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.ProfileCommerce.OwnedSystemSellerGuid", 900001);
        _config.LockedSystemSellerName = sConfigMgr->GetOption<std::string>(
            "LivingAzeroth.Economy.ProfileCommerce.OwnedSystemSellerName", "ExampleSeller");
        _config.CatalogSha256 = sConfigMgr->GetOption<std::string>(
            "LivingAzeroth.Economy.ProfileCommerce.CatalogSha256", "");
        _config.HumanAuctionMutationConfirmation = sConfigMgr->GetOption<std::string>(
            "LivingAzeroth.Economy.ProfileCommerce.Buyer.HumanAuctionMutationConfirmation", "");
        _config.ListingEnvironmentId = sConfigMgr->GetOption<std::string>(
            "LivingAzeroth.Economy.ProfileCommerce.Seller.EnvironmentId", "");
        _config.ListingRealmId = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.ProfileCommerce.Seller.RealmId", 0);
        _config.ListingPolicyVersion = sConfigMgr->GetOption<std::string>(
            "LivingAzeroth.Economy.ProfileCommerce.Seller.PolicyVersion", "");
        _config.RotationEnabled = sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.ProfileCommerce.Rotation.Enable", false);
        _config.RotationMaximumPerFaction = std::min<std::uint32_t>(
            sConfigMgr->GetOption<std::uint32_t>(
                "LivingAzeroth.Economy.ProfileCommerce.Rotation.MaxBotsPerFaction", 0),
            std::uint32_t(1));
        _config.RotationConfirmation = sConfigMgr->GetOption<std::string>(
            "LivingAzeroth.Economy.ProfileCommerce.Rotation.Confirmation", "");
        RefreshRuntimeBlock();
        if (reload)
            _elapsedMs = 0;
        LOG_INFO("living_azeroth.economy",
            "Profile commerce configuration {}: master={}, enabled={}, observeOnly={}, "
            "seller={}, buyer={}, humanAuctions={}, rotation={}, rotationMaxPerFaction={}, "
            "maxMutationsPerBoot={}, runtimeBlocked={}.",
            reload ? "reloaded" : "initialized", _config.MasterEnabled, policy.Enabled,
            policy.ObserveOnly, policy.SellerEnabled, policy.BuyerEnabled,
            policy.AllowHumanAuctions, _config.RotationEnabled,
            _config.RotationMaximumPerFaction, policy.MaxMutationsPerBoot, _runtimeBlocked);
    }

public:
    void OnStartup() override
    {
        _elapsedMs = 0;
        _mutationAttemptsThisBoot = 0;
        _attemptedSellerItemGuids.clear();
        _attemptedBuyerAuctionIds.clear();
        _ownedAiAccounts.clear();
        _ledgerReady = false;
        _systemSellerReady = false;
        _priceIndexReady = false;
        if (!_config.CommercePolicy.Enabled)
        {
            _runtimeBlocked = true;
            LOG_INFO("living_azeroth.economy",
                "Profile commerce actuator disabled (safe default): master={}.",
                _config.MasterEnabled);
            return;
        }

        _priceIndexReady = LivingAzeroth::Economy::FullPriceIndex::Instance().EnsureLoaded();
        if (!LoadOwnedAiAccounts())
            LOG_ERROR("living_azeroth.economy", "Profile commerce AI account cache is empty.");
        QueryResult schema = CharacterDatabase.Query(
            "SELECT schema_version FROM la_profile_commerce_schema WHERE singleton_id=1");
        _ledgerReady = schema && (*schema)[0].Get<std::uint32_t>() >= 1;
        _systemSellerReady = ValidateOwnedSystemSeller();
        RefreshRuntimeBlock();
        if (_runtimeBlocked)
        {
            LOG_ERROR("living_azeroth.economy",
                "Profile commerce failed closed: master={}, ledgerReady={}, systemSellerReady={}, "
                "catalogHashReady={}, aiAccounts={}, observeOnly={}, maxMutationsPerBoot={}.",
                _config.MasterEnabled, _ledgerReady, _systemSellerReady,
                _priceIndexReady && _config.CatalogSha256 ==
                    LivingAzeroth::Economy::FullPriceCatalogSha256,
                _ownedAiAccounts.size(), _config.CommercePolicy.ObserveOnly,
                _config.CommercePolicy.MaxMutationsPerBoot);
            return;
        }
        LOG_INFO("living_azeroth.economy",
            "Profile commerce actuator ready: observeOnly={}, seller={}, buyer={}, cycleMs={}, "
            "ownedAiAccounts={}, catalogItems={}, maxMutationsPerBoot={}.",
            _config.CommercePolicy.ObserveOnly, _config.CommercePolicy.SellerEnabled,
            _config.CommercePolicy.BuyerEnabled, _config.CycleMs, _ownedAiAccounts.size(),
            LivingAzeroth::Economy::FullPriceIndex::Instance().ApprovedRows().size(),
            _config.CommercePolicy.MaxMutationsPerBoot);
    }

    void OnUpdate(std::uint32_t diff) override
    {
        if (!_config.CommercePolicy.Enabled || _runtimeBlocked)
            return;
        _elapsedMs += diff;
        if (_elapsedMs < _config.CycleMs)
            return;
        _elapsedMs = 0;
        ReconcileSettlements();
        RunCycle();
    }

private:
    void RefreshRuntimeBlock()
    {
        bool const unsafeHumanMutation = !_config.CommercePolicy.ObserveOnly &&
            _config.CommercePolicy.BuyerEnabled &&
            _config.CommercePolicy.AllowHumanAuctions &&
            _config.HumanAuctionMutationConfirmation !=
                "I_ACCEPT_BOUNDED_HUMAN_AUCTIONS_V1";
        bool const mutationWithoutBootCap = !_config.CommercePolicy.ObserveOnly &&
            _config.CommercePolicy.MaxMutationsPerBoot == 0;
        bool const catalogMismatch = _config.CommercePolicy.RequireCatalogApproval &&
            (!_priceIndexReady || _config.CatalogSha256 !=
                LivingAzeroth::Economy::FullPriceCatalogSha256);
        bool const unsafeRotation = _config.RotationEnabled &&
            (_config.RotationMaximumPerFaction == 0 ||
                _config.RotationConfirmation != "I_ACCEPT_BOUNDED_COMMERCE_ROTATION_V1");
        bool const mutatingSeller = !_config.CommercePolicy.ObserveOnly &&
            _config.CommercePolicy.SellerEnabled;
        bool const unsafeListingAuthority = mutatingSeller &&
            (!LivingAzeroth::Economy::AutomatedListing::Ready() ||
             _config.ListingEnvironmentId.size() < 3 ||
             _config.ListingEnvironmentId.size() > 32 || _config.ListingRealmId == 0 ||
             _config.ListingPolicyVersion.size() < 3 ||
             _config.ListingPolicyVersion.size() > 64);
        _runtimeBlocked = !_config.MasterEnabled || !_config.CommercePolicy.Enabled ||
            _ownedAiAccounts.empty() || !_ledgerReady || !_systemSellerReady ||
            unsafeHumanMutation || mutationWithoutBootCap || catalogMismatch || unsafeRotation ||
            unsafeListingAuthority;
    }

    bool LoadOwnedAiAccounts()
    {
        QueryResult result = PlayerbotsDatabase.Query(
            "SELECT account_id FROM playerbots_account_type WHERE account_type IN (1,2) ORDER BY account_id");
        if (!result)
            return false;
        do
        {
            _ownedAiAccounts.insert(result->Fetch()[0].Get<std::uint32_t>());
        } while (result->NextRow());
        return !_ownedAiAccounts.empty();
    }

    bool ValidateOwnedSystemSeller() const
    {
        if (_config.LockedSystemSellerGuid == 0 || _config.LockedSystemSellerName.empty() ||
            _config.OwnedSystemSellerGuids.size() != 1 ||
            !_config.OwnedSystemSellerGuids.contains(_config.LockedSystemSellerGuid))
            return false;
        ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(_config.LockedSystemSellerGuid);
        CharacterCacheEntry const* cached = sCharacterCache->GetCharacterCacheByGuid(guid);
        return cached && cached->Name == _config.LockedSystemSellerName &&
            cached->AccountId != 0 && !_ownedAiAccounts.contains(cached->AccountId);
    }

    struct PendingSettlementRow
    {
        std::string ActionKey;
        std::string ActionKind;
        std::uint32_t CharacterGuid = 0;
        std::uint32_t AccountId = 0;
        std::uint32_t SellerGuid = 0;
        std::uint32_t HouseId = 0;
        std::uint32_t TargetId = 0;
        std::uint32_t ItemGuid = 0;
        std::uint32_t ItemEntry = 0;
        std::uint32_t ItemCount = 0;
        std::uint64_t TotalPrice = 0;
        std::uint64_t Deposit = 0;
        std::uint64_t Cut = 0;
        std::uint64_t WalletBefore = 0;
        std::uint64_t RecordedAt = 0;
        SettlementObservation Observed;
    };

    std::string MakeActionKey(char const* kind, std::uint32_t botGuid,
        std::uint32_t target) const
    {
        return std::string(kind) + ":" + std::to_string(botGuid) + ":" +
            std::to_string(target) + ":" + std::to_string(_mutationAttemptsThisBoot + 1) +
            ":" + std::to_string(GameTime::GetGameTime().count()) + ":" +
            std::to_string(GameTime::GetGameTimeMS().count());
    }

    static std::string EvidenceHash(std::string const& payload)
    {
        return ByteArrayToHexStr(Acore::Crypto::SHA256::GetDigestOf(payload));
    }

    static void RecordDispatch(std::string const& actionKey, char const* actionKind,
        std::uint32_t botGuid, std::uint32_t accountId, std::uint32_t sellerGuid,
        std::uint32_t houseId, std::uint32_t targetId, std::uint32_t itemGuid,
        std::uint32_t itemEntry, std::uint32_t itemCount, std::uint64_t totalPrice,
        std::uint64_t deposit, std::uint64_t cut, std::uint64_t walletBefore)
    {
        std::uint64_t const recordedAt = GameTime::GetGameTime().count();
        std::string const evidence = EvidenceHash(
            actionKey + "|dispatched|" + actionKind + "|" +
            std::to_string(botGuid) + "|" + std::to_string(accountId) + "|" +
            std::to_string(sellerGuid) + "|" + std::to_string(houseId) + "|" +
            std::to_string(targetId) + "|" + std::to_string(itemGuid) + "|" +
            std::to_string(itemEntry) + "|" + std::to_string(itemCount) + "|" +
            std::to_string(totalPrice) + "|" + std::to_string(deposit) + "|" +
            std::to_string(cut) + "|" + std::to_string(walletBefore) + "|" +
            std::to_string(recordedAt));

        CharacterDatabaseTransaction transaction = CharacterDatabase.BeginTransaction();
        transaction->Append(DispatchLedgerSql,
            actionKey, actionKind, botGuid, accountId, sellerGuid, houseId, targetId,
            itemGuid, itemEntry, itemCount, totalPrice, deposit, cut, walletBefore,
            recordedAt, evidence);
        if (std::string_view(actionKind) == "auction_buyout")
        {
            std::uint64_t const utcDay = recordedAt / DAY;
            transaction->Append(DailyUsageSql,
                botGuid, utcDay, totalPrice, totalPrice);
        }
        CharacterDatabase.DirectCommitTransaction(transaction);
    }

    static void RecordSettlement(PendingSettlementRow const& row,
        SettlementOutcome outcome, char const* reason)
    {
        char const* phase = outcome == SettlementOutcome::Verified ? "verified" : "rejected";
        std::uint32_t const auctionId =
            row.ActionKind == "auction_list" ? row.Observed.ListingAuctionId : row.TargetId;
        std::uint64_t const buyerDebit =
            row.ActionKind == "auction_buyout" && outcome == SettlementOutcome::Verified ?
                row.TotalPrice : 0;
        std::uint64_t const sellerMail =
            row.ActionKind == "auction_buyout" ? row.Observed.SellerMailMoney : 0;
        std::uint64_t const recordedAt = GameTime::GetGameTime().count();
        std::string const evidence = EvidenceHash(
            row.ActionKey + "|" + phase + "|" + reason + "|" +
            std::to_string(auctionId) + "|" + std::to_string(row.ItemGuid) + "|" +
            std::to_string(buyerDebit) + "|" + std::to_string(sellerMail) + "|" +
            std::to_string(row.Deposit) + "|" + std::to_string(row.Cut) + "|" +
            std::to_string(recordedAt));

        CharacterDatabaseTransaction transaction = CharacterDatabase.BeginTransaction();
        transaction->Append(SettlementLedgerSql,
            row.ActionKey, phase, row.ActionKind, row.CharacterGuid, row.AccountId,
            row.SellerGuid, row.HouseId, row.TargetId, row.ItemGuid, row.ItemEntry,
            row.ItemCount, row.TotalPrice, row.Deposit, row.Cut, row.WalletBefore,
            recordedAt, evidence);
        transaction->Append(SettlementEvidenceSql,
            row.ActionKey, phase, auctionId, row.ItemGuid, buyerDebit, sellerMail,
            row.Deposit, row.Cut, evidence, recordedAt);
        CharacterDatabase.CommitTransaction(transaction);
        LOG_INFO("living_azeroth.economy",
            "Profile commerce settlement outcome={} reason={} actionKey={} actionKind={} "
            "auction={} itemGuid={} buyerDebit={} sellerMail={} deposit={} cut={}.",
            phase, reason, row.ActionKey, row.ActionKind, auctionId, row.ItemGuid,
            buyerDebit, sellerMail, row.Deposit, row.Cut);
    }

    LivingAzeroth::Economy::AutomatedListing::Pending MakeListingPending(
        PendingSettlementRow const& row) const
    {
        std::uint32_t const startBid = static_cast<std::uint32_t>(row.TotalPrice * 80 / 100);
        std::uint32_t const durationSeconds = static_cast<std::uint32_t>(
            MIN_AUCTION_TIME * sWorld->getRate(RATE_AUCTION_TIME));
        std::string const identity = _config.ListingEnvironmentId + "|" +
            std::to_string(_config.ListingRealmId) + "|auction_list|" +
            _config.ListingPolicyVersion + "|" + std::to_string(row.CharacterGuid) + "|" +
            std::to_string(row.ItemGuid) + "|" + std::to_string(row.ItemEntry) + "|" +
            std::to_string(row.ItemCount) + "|" + std::to_string(row.HouseId) + "|" +
            std::to_string(startBid) + "|" + std::to_string(row.TotalPrice) + "|" +
            std::to_string(row.Deposit) + "|" + std::to_string(durationSeconds);
        LivingAzeroth::Economy::AutomatedListing::Pending pending;
        pending.ActionId = row.ActionKey;
        pending.EnvironmentId = _config.ListingEnvironmentId;
        pending.RealmId = _config.ListingRealmId;
        pending.PolicyVersion = _config.ListingPolicyVersion;
        pending.PayloadSha256 = EvidenceHash(identity + "|" + row.ActionKey);
        pending.SellerGuid = row.CharacterGuid;
        pending.SellerAccount = row.AccountId;
        pending.SourceItemGuid = row.ItemGuid;
        pending.ItemEntry = row.ItemEntry;
        pending.ItemCount = row.ItemCount;
        pending.HouseId = row.HouseId;
        pending.StartBid = startBid;
        pending.Buyout = static_cast<std::uint32_t>(row.TotalPrice);
        pending.Deposit = static_cast<std::uint32_t>(row.Deposit);
        pending.DurationSeconds = durationSeconds;
        return pending;
    }

    void ReconcileSettlements()
    {
        std::uint64_t const now = GameTime::GetGameTime().count();
        QueryResult result = CharacterDatabase.Query(
            "SELECT d.action_key,d.action_kind,d.character_guid,d.account_id,d.seller_guid,"
            "d.house_id,d.target_id,d.item_guid,d.item_entry,d.item_count,"
            "d.total_price_copper,d.deposit_copper,d.cut_copper,d.wallet_before_copper,"
            "d.recorded_at_unix,"
            "(SELECT COUNT(*) FROM auctionhouse a JOIN item_instance ii ON ii.guid=a.itemguid "
            " WHERE d.action_kind='auction_list' AND a.itemguid=d.item_guid "
            " AND a.itemowner=d.character_guid AND a.houseid=d.house_id "
            " AND a.buyoutprice=d.total_price_copper "
            " AND a.startbid=(d.total_price_copper*80 DIV 100) "
            " AND ii.itemEntry=d.item_entry AND ii.count=d.item_count),"
            "(SELECT COUNT(*) FROM auctionhouse a WHERE a.itemguid=d.item_guid),"
            "COALESCE((SELECT MIN(a.id) FROM auctionhouse a WHERE a.itemguid=d.item_guid),0),"
            "COALESCE((SELECT MIN(a.deposit) FROM auctionhouse a WHERE a.itemguid=d.item_guid),0),"
            "(SELECT COUNT(*) FROM auctionhouse a WHERE a.id=d.target_id),"
            "(SELECT COUNT(*) FROM mail m JOIN mail_items mi ON mi.mail_id=m.id "
            " WHERE d.action_kind='auction_buyout' AND m.receiver=d.character_guid "
            " AND mi.item_guid=d.item_guid AND m.messageType=2 AND m.has_items<>0 "
            " AND m.subject=CONCAT(d.item_entry,':0:1:',d.target_id,':',d.item_count)),"
            "(SELECT COUNT(*) FROM mail m WHERE d.action_kind='auction_buyout' "
            " AND m.receiver=d.seller_guid AND m.messageType=2 AND m.has_items=0 "
            " AND m.subject=CONCAT(d.item_entry,':0:2:',d.target_id,':',d.item_count)),"
            "COALESCE((SELECT SUM(m.money) FROM mail m WHERE d.action_kind='auction_buyout' "
            " AND m.receiver=d.seller_guid AND m.messageType=2 AND m.has_items=0 "
            " AND m.subject=CONCAT(d.item_entry,':0:2:',d.target_id,':',d.item_count)),0) "
            "FROM la_profile_commerce_ledger d LEFT JOIN la_profile_commerce_ledger final "
            " ON final.action_key=d.action_key AND final.phase IN ('verified','rejected') "
            "WHERE d.phase='dispatched' AND final.ledger_id IS NULL "
            "AND d.recorded_at_unix+5<={} ORDER BY d.ledger_id LIMIT 100",
            now);
        if (!result)
            return;

        do
        {
            Field* fields = result->Fetch();
            PendingSettlementRow row;
            row.ActionKey = fields[0].Get<std::string>();
            row.ActionKind = fields[1].Get<std::string>();
            row.CharacterGuid = fields[2].Get<std::uint32_t>();
            row.AccountId = fields[3].Get<std::uint32_t>();
            row.SellerGuid = fields[4].Get<std::uint32_t>();
            row.HouseId = fields[5].Get<std::uint32_t>();
            row.TargetId = fields[6].Get<std::uint32_t>();
            row.ItemGuid = fields[7].Get<std::uint32_t>();
            row.ItemEntry = fields[8].Get<std::uint32_t>();
            row.ItemCount = fields[9].Get<std::uint32_t>();
            row.TotalPrice = fields[10].Get<std::uint64_t>();
            row.Deposit = fields[11].Get<std::uint64_t>();
            row.Cut = fields[12].Get<std::uint64_t>();
            row.WalletBefore = fields[13].Get<std::uint64_t>();
            row.RecordedAt = fields[14].Get<std::uint64_t>();
            row.Observed.Action = row.ActionKind == "auction_list" ?
                SettlementActionKind::AuctionList : SettlementActionKind::AuctionBuyout;
            row.Observed.AgeSeconds = now >= row.RecordedAt ? now - row.RecordedAt : 0;
            row.Observed.TotalPrice = row.TotalPrice;
            row.Observed.Deposit = row.Deposit;
            row.Observed.Cut = row.Cut;
            row.Observed.ExactListingCount = fields[15].Get<std::uint32_t>();
            row.Observed.ItemAuctionReferences = fields[16].Get<std::uint32_t>();
            row.Observed.ListingAuctionId = fields[17].Get<std::uint32_t>();
            row.Observed.ListingDeposit = fields[18].Get<std::uint64_t>();
            row.Observed.PurchaseAuctionReferences = fields[19].Get<std::uint32_t>();
            row.Observed.WonMailCount = fields[20].Get<std::uint32_t>();
            row.Observed.SellerMailCount = fields[21].Get<std::uint32_t>();
            row.Observed.SellerMailMoney = fields[22].Get<std::uint64_t>();

            if (row.ActionKind == "auction_list")
            {
                auto const pending = MakeListingPending(row);
                auto const receipt =
                    LivingAzeroth::Economy::AutomatedListing::ReadReceipt(pending);
                if (receipt == LivingAzeroth::Economy::AutomatedListing::ReceiptState::Applied)
                {
                    QueryResult receiptRow = CharacterDatabase.Query(
                        "SELECT auction_id,deposit_copper FROM "
                        "la_outlands_auction_listing_receipt WHERE action_id='{}'",
                        row.ActionKey);
                    if (!receiptRow)
                    {
                        _runtimeBlocked = true;
                        LOG_FATAL("living_azeroth.economy",
                            "Profile seller quarantined actionId={} reason=receipt_readback_lost.",
                            row.ActionKey);
                        continue;
                    }
                    Field* receiptFields = receiptRow->Fetch();
                    row.Observed.ListingAuctionId = receiptFields[0].Get<std::uint32_t>();
                    row.Observed.ListingDeposit = receiptFields[1].Get<std::uint64_t>();
                    RecordSettlement(row, SettlementOutcome::Verified,
                        "destination_listing_receipt_applied");
                    continue;
                }
                if (receipt == LivingAzeroth::Economy::AutomatedListing::ReceiptState::Conflict)
                {
                    _runtimeBlocked = true;
                    LOG_FATAL("living_azeroth.economy",
                        "Profile seller quarantined actionId={} reason=receipt_payload_conflict.",
                        row.ActionKey);
                    continue;
                }
                if (row.Observed.AgeSeconds >= 30)
                {
                    QueryResult location = CharacterDatabase.Query(
                        "SELECT "
                        "(SELECT COUNT(*) FROM character_inventory ci WHERE ci.guid={} "
                        " AND ci.item={}),"
                        "(SELECT COUNT(*) FROM auctionhouse a WHERE a.itemguid={})",
                        row.CharacterGuid, row.ItemGuid, row.ItemGuid);
                    std::uint32_t inventoryReferences = 0;
                    std::uint32_t auctionReferences = 0;
                    if (location)
                    {
                        Field* locationFields = location->Fetch();
                        inventoryReferences = locationFields[0].Get<std::uint32_t>();
                        auctionReferences = locationFields[1].Get<std::uint32_t>();
                    }
                    if (!location || inventoryReferences != 1 || auctionReferences != 0)
                    {
                        _runtimeBlocked = true;
                        LOG_FATAL("living_azeroth.economy",
                            "Profile seller quarantined actionId={} reason=missing_receipt_ambiguous_state "
                            "inventoryRefs={} auctionRefs={}.", row.ActionKey,
                            inventoryReferences, auctionReferences);
                    }
                }
                continue;
            }

            auto const decision = PlanSettlement(row.Observed);
            if (decision.Outcome != SettlementOutcome::Pending)
                RecordSettlement(row, decision.Outcome, decision.Reason);
        } while (result->NextRow());
    }

    static bool Available(Player* bot)
    {
        return bot && bot->GetSession() && bot->IsInWorld() &&
            !bot->IsBeingTeleported() && !bot->IsInCombat() &&
            !bot->GetGroup() && sRandomPlayerbotMgr.IsRandomBot(bot);
    }

    static Creature* FindAuctioneer(Player* bot, PlayerbotAI* ai)
    {
        if (!bot || !ai)
            return nullptr;
        GuidVector const nearby = ai->GetAiObjectContext()->GetValue<GuidVector>("nearest npcs")->Get();
        for (ObjectGuid const& guid : nearby)
        {
            if (Creature* auctioneer = bot->GetNPCIfCanInteractWith(
                    guid, UNIT_NPC_FLAG_AUCTIONEER))
                return auctioneer;
        }
        return nullptr;
    }

    static CommerceRotation::Faction RotationFaction(Player const* bot)
    {
        if (!bot)
            return CommerceRotation::Faction::Other;
        if (bot->GetTeamId() == TEAM_ALLIANCE)
            return CommerceRotation::Faction::Alliance;
        if (bot->GetTeamId() == TEAM_HORDE)
            return CommerceRotation::Faction::Horde;
        return CommerceRotation::Faction::Other;
    }

    void MaintainCommerceRotation(std::vector<Player*> const& bots)
    {
        if (!_config.RotationEnabled)
            return;
        std::vector<CommerceRotation::Candidate> candidates;
        candidates.reserve(bots.size());
        for (Player* bot : bots)
        {
            PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
            candidates.push_back({
                bot->GetGUID().GetCounter(), RotationFaction(bot), true,
                FindAuctioneer(bot, ai) != nullptr
            });
        }
        auto decisions = CommerceRotation::Plan(
            candidates, _config.RotationMaximumPerFaction);
        for (CommerceRotation::Decision const& decision : decisions)
        {
            auto found = std::find_if(bots.begin(), bots.end(), [&](Player const* bot)
            {
                return bot->GetGUID().GetCounter() == decision.guid;
            });
            if (found == bots.end())
                continue;
            Player* bot = *found;
            CommerceTarget const& target = decision.faction == CommerceRotation::Faction::Alliance
                ? AllianceCommerceTarget : HordeCommerceTarget;
            bool const teleported = bot->TeleportTo(
                target.Map, target.X, target.Y, target.Z, target.O);
            LOG_INFO("living_azeroth.economy",
                "Commerce rotation teleport result={} bot={} guid={} faction={} map={} x={} y={}.",
                teleported ? "PASS" : "FAIL", bot->GetName(), decision.guid,
                decision.faction == CommerceRotation::Faction::Alliance ? "Alliance" : "Horde",
                target.Map, target.X, target.Y);
        }
    }

    static std::uint32_t ListingCount(ObjectGuid owner)
    {
        std::uint32_t count = 0;
        for (AuctionHouseId houseId : AuctionHouses)
        {
            AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(houseId);
            for (auto const& [id, auction] : house->GetAuctions())
            {
                (void)id;
                if (auction && auction->owner == owner)
                    ++count;
            }
        }
        return count;
    }

    static std::uint64_t VendorReference(ItemTemplate const* itemTemplate)
    {
        if (!itemTemplate)
            return 0;
        return std::max<std::uint64_t>(itemTemplate->BuyPrice,
            SaturatingMultiply(itemTemplate->SellPrice, 4));
    }

    static std::uint64_t MarketReference(AuctionHouseObject* house,
        std::uint32_t itemEntry, ItemTemplate const* itemTemplate,
        std::uint32_t excludedAuctionId = 0)
    {
        std::uint64_t const trustedStatic =
            LivingAzeroth::Economy::FullPriceIndex::Instance().ProcurementUnitPrice(itemEntry);
        if (trustedStatic != 0)
            return trustedStatic;
        std::vector<std::uint64_t> samples;
        if (house)
        {
            for (auto const& [id, auction] : house->GetAuctions())
            {
                (void)id;
                if (auction && auction->Id != excludedAuctionId &&
                    auction->item_template == itemEntry && auction->itemCount != 0 &&
                    auction->buyout != 0)
                    samples.push_back(auction->buyout / auction->itemCount);
            }
        }
        std::uint64_t const vendor = VendorReference(itemTemplate);
        if (samples.empty())
            return vendor;
        std::sort(samples.begin(), samples.end());
        std::uint64_t market = samples[samples.size() / 2];
        if (vendor != 0)
            market = std::clamp(market, vendor * 65 / 100, vendor * 175 / 100);
        return market;
    }

    static std::vector<Item*> BagItems(Player* bot)
    {
        std::vector<Item*> items;
        for (std::uint8_t slot = INVENTORY_SLOT_ITEM_START;
             slot < INVENTORY_SLOT_ITEM_END; ++slot)
        {
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                items.push_back(item);
        }
        for (std::uint8_t bag = INVENTORY_SLOT_BAG_START;
             bag < INVENTORY_SLOT_BAG_END; ++bag)
        {
            Bag* container = bot->GetBagByPos(bag);
            if (!container)
                continue;
            for (std::uint8_t slot = 0; slot < container->GetBagSize(); ++slot)
            {
                if (Item* item = bot->GetItemByPos(bag, slot))
                    items.push_back(item);
            }
        }
        std::sort(items.begin(), items.end(), [](Item const* left, Item const* right)
        {
            if (left->GetEntry() != right->GetEntry())
                return left->GetEntry() < right->GetEntry();
            return left->GetGUID().GetCounter() < right->GetGUID().GetCounter();
        });
        return items;
    }

    static Item* SellerItem(PlayerbotAI* ai, Player* bot)
    {
        for (Item* item : BagItems(bot))
        {
            ItemTemplate const* itemTemplate = item->GetTemplate();
            if (!itemTemplate ||
                !LivingAzeroth::Economy::FullPriceIndex::Instance().Approved(item->GetEntry()) ||
                !item->CanBeTraded() || item->IsNotEmptyBag() ||
                itemTemplate->Class == ITEM_CLASS_QUEST ||
                itemTemplate->Bonding == BIND_WHEN_PICKED_UP ||
                itemTemplate->Bonding == BIND_QUEST_ITEM ||
                itemTemplate->HasFlag(ITEM_FLAG_CONJURED) ||
                item->GetUInt32Value(ITEM_FIELD_DURATION) != 0)
                continue;
            ItemUsage const usage = ai->GetAiObjectContext()->GetValue<ItemUsage>(
                "item usage", item->GetEntry())->Get();
            if (usage == ITEM_USAGE_AH)
            {
                if (LivingAzeroth::Economy::AutomatedListing::Ready() &&
                    LivingAzeroth::Economy::AutomatedListing::IsAutomationResaleBlocked(
                        item->GetGUID().GetCounter()))
                    continue;
                return item;
            }
        }
        return nullptr;
    }

    bool TrySeller(Player* bot, PlayerbotAI* ai, Creature* auctioneer,
        std::uint32_t actionsThisCycle)
    {
        Item* item = SellerItem(ai, bot);
        if (!item)
            return false;
        AuctionHouseObject* house = sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction());
        AuctionHouseEntry const* houseEntry =
            AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(auctioneer->GetFaction());
        ItemTemplate const* itemTemplate = item->GetTemplate();
        if (!house || !houseEntry || !itemTemplate)
            return false;
        std::uint32_t const itemCount = item->GetCount();
        std::uint64_t const reference = MarketReference(house, item->GetEntry(), itemTemplate);
        std::uint32_t const deposit = sAuctionMgr->GetAuctionDeposit(
            houseEntry, MIN_AUCTION_TIME, item, itemCount);
        SellerObservation observed;
        observed.OwnedAi = true;
        observed.AtAuctioneer = true;
        observed.CatalogApproved =
            LivingAzeroth::Economy::FullPriceIndex::Instance().Approved(item->GetEntry());
        observed.Tradable = item->CanBeTraded();
        observed.Bound = item->IsSoulBound();
        observed.QuestItem = itemTemplate->Class == ITEM_CLASS_QUEST ||
            itemTemplate->Bonding == BIND_QUEST_ITEM;
        observed.Conjured = itemTemplate->HasFlag(ITEM_FLAG_CONJURED);
        observed.HasDuration = item->GetUInt32Value(ITEM_FIELD_DURATION) != 0;
        observed.NeededByProfile = false;
        observed.DuplicateAction = _attemptedSellerItemGuids.contains(item->GetGUID().GetCounter());
        observed.ExistingListings = ListingCount(bot->GetGUID());
        observed.SellerActionsThisCycle = actionsThisCycle;
        observed.MutationsThisBoot = _mutationAttemptsThisBoot;
        observed.ReferenceUnitPrice = reference;
        observed.ProposedUnitPrice = reference;
        observed.Money = bot->GetMoney();
        observed.Deposit = deposit;
        Decision const decision = PlanSeller(_config.CommercePolicy, observed);
        if (!decision.WouldAct)
            return false;

        std::uint32_t const itemEntry = item->GetEntry();
        std::uint64_t const lotPrice = SaturatingMultiply(reference, itemCount);
        LOG_INFO("living_azeroth.economy",
            "Profile seller decision={} botGuid={} item={} count={} unitPrice={} existingListings={}.",
            ReasonName(decision.Reason), bot->GetGUID().GetCounter(), itemEntry,
            itemCount, reference, observed.ExistingListings);
        if (!decision.MayMutate || lotPrice == 0 || lotPrice > MAX_MONEY_AMOUNT)
            return true;

        std::uint32_t const botGuid = bot->GetGUID().GetCounter();
        std::uint32_t const startBid = static_cast<std::uint32_t>(lotPrice * 80 / 100);
        std::uint32_t const durationSeconds = static_cast<std::uint32_t>(
            MIN_AUCTION_TIME * sWorld->getRate(RATE_AUCTION_TIME));
        std::string const identity = _config.ListingEnvironmentId + "|" +
            std::to_string(_config.ListingRealmId) + "|auction_list|" +
            _config.ListingPolicyVersion + "|" + std::to_string(botGuid) + "|" +
            std::to_string(item->GetGUID().GetCounter()) + "|" +
            std::to_string(itemEntry) + "|" + std::to_string(itemCount) + "|" +
            std::to_string(houseEntry->houseId) + "|" + std::to_string(startBid) + "|" +
            std::to_string(lotPrice) + "|" + std::to_string(deposit) + "|" +
            std::to_string(durationSeconds);
        std::string const actionKey = "outlands:" + _config.ListingEnvironmentId + ":" +
            std::to_string(_config.ListingRealmId) + ":auction-list:" +
            std::to_string(botGuid) + ":" + std::to_string(item->GetGUID().GetCounter()) +
            ":" + EvidenceHash(identity).substr(0, 16);
        std::string const payloadSha256 = EvidenceHash(identity + "|" + actionKey);
        LivingAzeroth::Economy::AutomatedListing::Pending pending;
        pending.ActionId = actionKey;
        pending.EnvironmentId = _config.ListingEnvironmentId;
        pending.RealmId = _config.ListingRealmId;
        pending.PolicyVersion = _config.ListingPolicyVersion;
        pending.PayloadSha256 = payloadSha256;
        pending.SellerGuid = botGuid;
        pending.SellerAccount = bot->GetSession()->GetAccountId();
        pending.SourceItemGuid = item->GetGUID().GetCounter();
        pending.ItemEntry = itemEntry;
        pending.ItemCount = itemCount;
        pending.HouseId = houseEntry->houseId;
        pending.StartBid = startBid;
        pending.Buyout = static_cast<std::uint32_t>(lotPrice);
        pending.Deposit = deposit;
        pending.DurationSeconds = durationSeconds;
        auto const receipt = LivingAzeroth::Economy::AutomatedListing::ReadReceipt(pending);
        if (receipt == LivingAzeroth::Economy::AutomatedListing::ReceiptState::Applied)
        {
            _attemptedSellerItemGuids.insert(item->GetGUID().GetCounter());
            LOG_INFO("living_azeroth.economy",
                "Profile seller duplicate suppressed actionId={} sellerGuid={} itemGuid={}.",
                actionKey, botGuid, item->GetGUID().GetCounter());
            return true;
        }
        if (receipt != LivingAzeroth::Economy::AutomatedListing::ReceiptState::Missing)
        {
            _runtimeBlocked = true;
            LOG_FATAL("living_azeroth.economy",
                "Profile seller quarantined actionId={} sellerGuid={} itemGuid={} receiptState={}.",
                actionKey, botGuid, item->GetGUID().GetCounter(),
                static_cast<std::uint32_t>(receipt));
            return true;
        }
        if (!LivingAzeroth::Economy::AutomatedListing::Register(pending))
        {
            _runtimeBlocked = true;
            LOG_FATAL("living_azeroth.economy",
                "Profile seller pending registration conflict actionId={} sellerGuid={} itemGuid={}.",
                actionKey, botGuid, item->GetGUID().GetCounter());
            return true;
        }
        _attemptedSellerItemGuids.insert(item->GetGUID().GetCounter());
        ++_mutationAttemptsThisBoot;
        RecordDispatch(actionKey, "auction_list", botGuid,
            bot->GetSession()->GetAccountId(), botGuid, houseEntry->houseId, 0,
            item->GetGUID().GetCounter(), itemEntry, itemCount, lotPrice,
            deposit, 0, bot->GetMoney());
        QueryResult intent = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM la_profile_commerce_ledger WHERE "
            "action_key='{}' AND phase='dispatched' AND action_kind='auction_list' "
            "AND character_guid={} AND item_guid={}", actionKey, botGuid,
            item->GetGUID().GetCounter());
        if (!intent || intent->Fetch()[0].Get<std::uint32_t>() != 1)
        {
            LivingAzeroth::Economy::AutomatedListing::Forget(
                botGuid, item->GetGUID().GetCounter());
            _runtimeBlocked = true;
            LOG_FATAL("living_azeroth.economy",
                "Profile seller blocked actionId={} reason=durable_intent_readback_failed.",
                actionKey);
            return true;
        }
        WorldPacket packet(CMSG_AUCTION_SELL_ITEM, 64);
        packet << auctioneer->GetGUID() << std::uint32_t(1)
               << item->GetGUID() << itemCount
               << startBid
               << std::uint32_t(lotPrice)
               << std::uint32_t(MIN_AUCTION_TIME / MINUTE);
        bot->GetSession()->HandleAuctionSellItem(packet);
        LOG_INFO("living_azeroth.economy",
            "Profile seller handled synchronously botGuid={} item={} count={} lotPrice={} "
            "deposit={} attempt={}.",
            botGuid, itemEntry, itemCount, lotPrice, deposit, _mutationAttemptsThisBoot);
        return true;
    }

    OwnerKind ClassifyOwner(ObjectGuid owner, std::uint32_t& account) const
    {
        std::uint32_t const guid = owner.GetCounter();
        CharacterCacheEntry const* cached = sCharacterCache->GetCharacterCacheByGuid(owner);
        account = cached ? cached->AccountId : 0;
        if (_config.OwnedSystemSellerGuids.contains(guid))
            return OwnerKind::OwnedSystem;
        if (account != 0 && _ownedAiAccounts.contains(account))
            return OwnerKind::OwnedAi;
        return cached ? OwnerKind::Human : OwnerKind::Unknown;
    }

    static bool UsefulPurchase(PlayerbotAI* ai, std::uint32_t itemEntry)
    {
        ItemUsage const usage = ai->GetAiObjectContext()->GetValue<ItemUsage>(
            "item usage", itemEntry)->Get();
        return usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE ||
            usage == ITEM_USAGE_SKILL || usage == ITEM_USAGE_USE ||
            usage == ITEM_USAGE_AMMO;
    }

    static std::string DailyEvent(char const* kind)
    {
        std::uint64_t const utcDay =
            static_cast<std::uint64_t>(GameTime::GetGameTime().count()) / DAY;
        return "living_azeroth_ah_" + std::string(kind) + "_" + std::to_string(utcDay);
    }

    bool TryBuyer(Player* bot, PlayerbotAI* ai, Creature* auctioneer,
        std::uint32_t actionsThisCycle)
    {
        AuctionHouseObject* house = sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction());
        AuctionEntry* selected = nullptr;
        Decision selectedDecision;
        std::uint32_t selectedSellerAccount = 0;
        std::uint32_t const botGuid = bot->GetGUID().GetCounter();
        std::uint32_t const purchasesToday = sRandomPlayerbotMgr.GetPersistentEventValue(
            botGuid, DailyEvent("buy_count"));
        std::uint32_t const spentToday = sRandomPlayerbotMgr.GetPersistentEventValue(
            botGuid, DailyEvent("buy_spend"));

        for (auto const& [id, auction] : house->GetAuctions())
        {
            (void)id;
            if (!auction || auction->buyout == 0 || auction->itemCount == 0 ||
                auction->bid != 0 || auction->bidder)
                continue;
            std::uint32_t sellerAccount = 0;
            OwnerKind const sellerKind = ClassifyOwner(auction->owner, sellerAccount);
            ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(auction->item_template);
            BuyerObservation observed;
            observed.OwnedAi = true;
            observed.AtAuctioneer = true;
            observed.CatalogApproved =
                LivingAzeroth::Economy::FullPriceIndex::Instance().Approved(
                    auction->item_template);
            observed.UsefulToProfile = UsefulPurchase(ai, auction->item_template);
            observed.SellerKind = sellerKind;
            observed.BuyerGuid = botGuid;
            observed.SellerGuid = auction->owner.GetCounter();
            observed.BuyerAccount = bot->GetSession()->GetAccountId();
            observed.SellerAccount = sellerAccount;
            observed.ItemCount = auction->itemCount;
            observed.PurchasesToday = purchasesToday;
            observed.BuyerActionsThisCycle = actionsThisCycle;
            observed.MutationsThisBoot = _mutationAttemptsThisBoot;
            observed.DuplicateAction = _attemptedBuyerAuctionIds.contains(auction->Id);
            observed.ReferenceUnitPrice = MarketReference(
                house, auction->item_template, itemTemplate, auction->Id);
            observed.Buyout = auction->buyout;
            observed.Money = bot->GetMoney();
            observed.SpentToday = spentToday;
            Decision const decision = PlanBuyer(_config.CommercePolicy, observed);
            if (!decision.WouldAct)
                continue;
            if (!selected || auction->buyout < selected->buyout ||
                (auction->buyout == selected->buyout && auction->Id < selected->Id))
            {
                selected = auction;
                selectedDecision = decision;
                selectedSellerAccount = sellerAccount;
            }
        }

        if (!selected || !selected->auctionHouseEntry)
            return false;
        std::uint32_t const auctionId = selected->Id;
        std::uint32_t const itemEntry = selected->item_template;
        std::uint32_t const buyout = selected->buyout;
        std::uint32_t const sellerGuid = selected->owner.GetCounter();
        std::uint64_t const auctionCut = CalculateAuctionCutForBuyout(
            buyout, selected->auctionHouseEntry->cutPercent,
            sWorld->getRate(RATE_AUCTION_CUT));
        LOG_INFO("living_azeroth.economy",
            "Profile buyer decision={} botGuid={} auction={} sellerGuid={} sellerAccount={} "
            "item={} buyout={} purchasesToday={} spentToday={}.",
            ReasonName(selectedDecision.Reason), botGuid, auctionId, sellerGuid,
            selectedSellerAccount, itemEntry, buyout, purchasesToday, spentToday);
        if (!selectedDecision.MayMutate)
            return true;

        std::string const actionKey = MakeActionKey("buy", botGuid, auctionId);
        _attemptedBuyerAuctionIds.insert(auctionId);
        ++_mutationAttemptsThisBoot;
        sRandomPlayerbotMgr.SetPersistentEventValue(botGuid, DailyEvent("buy_count"),
            purchasesToday + 1, PersistentEventSeconds);
        sRandomPlayerbotMgr.SetPersistentEventValue(botGuid, DailyEvent("buy_spend"),
            spentToday + buyout, PersistentEventSeconds);
        RecordDispatch(actionKey, "auction_buyout", botGuid,
            bot->GetSession()->GetAccountId(), sellerGuid,
            static_cast<std::uint32_t>(selected->houseId), auctionId,
            selected->item_guid.GetCounter(), itemEntry, selected->itemCount,
            buyout, selected->deposit, auctionCut, bot->GetMoney());
        WorldPacket* packet = new WorldPacket(CMSG_AUCTION_PLACE_BID, 24);
        *packet << auctioneer->GetGUID() << auctionId << buyout;
        bot->GetSession()->QueuePacket(packet);
        LOG_INFO("living_azeroth.economy",
            "Profile buyer queued botGuid={} auction={} item={} buyout={} attempt={}.",
            botGuid, auctionId, itemEntry, buyout, _mutationAttemptsThisBoot);
        return true;
    }

    void RunCycle()
    {
        std::vector<Player*> bots;
        for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBots())
        {
            (void)guid;
            if (Available(bot))
                bots.push_back(bot);
        }
        std::sort(bots.begin(), bots.end(), [](Player const* left, Player const* right)
        {
            return left->GetGUID().GetCounter() < right->GetGUID().GetCounter();
        });

        MaintainCommerceRotation(bots);

        std::uint32_t sellerActions = 0;
        std::uint32_t buyerActions = 0;
        for (Player* bot : bots)
        {
            PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
            Creature* auctioneer = FindAuctioneer(bot, ai);
            if (!auctioneer)
                continue;
            if (_config.CommercePolicy.SellerEnabled &&
                sellerActions < _config.CommercePolicy.MaxSellerActionsPerCycle &&
                TrySeller(bot, ai, auctioneer, sellerActions))
                ++sellerActions;
            if (_config.CommercePolicy.BuyerEnabled &&
                buyerActions < _config.CommercePolicy.MaxBuyerActionsPerCycle &&
                TryBuyer(bot, ai, auctioneer, buyerActions))
                ++buyerActions;
            if (sellerActions >= _config.CommercePolicy.MaxSellerActionsPerCycle &&
                buyerActions >= _config.CommercePolicy.MaxBuyerActionsPerCycle)
                break;
        }
        LOG_INFO("living_azeroth.economy",
            "Profile commerce cycle complete: availableBots={}, sellerActions={}, buyerActions={}, "
            "observeOnly={}, mutationAttemptsThisBoot={}.",
            bots.size(), sellerActions, buyerActions, _config.CommercePolicy.ObserveOnly,
            _mutationAttemptsThisBoot);
    }

    RuntimeConfig _config;
    std::unordered_set<std::uint32_t> _ownedAiAccounts;
    std::unordered_set<std::uint32_t> _attemptedSellerItemGuids;
    std::unordered_set<std::uint32_t> _attemptedBuyerAuctionIds;
    std::uint32_t _elapsedMs = 0;
    std::uint32_t _mutationAttemptsThisBoot = 0;
    bool _ledgerReady = false;
    bool _systemSellerReady = false;
    bool _priceIndexReady = false;
    bool _runtimeBlocked = true;
};
}

void AddLivingAzerothProfileCommerceActuatorScripts()
{
    new LivingAzerothProfileCommerceActuator();
}
