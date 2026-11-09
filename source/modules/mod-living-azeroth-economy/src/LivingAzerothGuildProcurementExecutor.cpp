#include "LivingAzerothCommerceSettlementPlan.h"
#include "LivingAzerothDestinationReceiptPlan.h"
#include "LivingAzerothEconomyPlan.h"
#include "LivingAzerothFullPriceIndex.h"
#include "LivingAzerothGuildProcurementExecutionPlan.h"
#include "LivingAzerothGuildProcurementPlan.h"
#include "LivingAzerothProcurementBuyerSelectionPlan.h"
#include "LivingAzerothWriterCompatibilityPlan.h"

#include "AuctionHouseMgr.h"
#include "CharacterCache.h"
#include "Config.h"
#include "Creature.h"
#include "CryptoHash.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
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
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
namespace Procurement = LivingAzeroth::Economy::GuildProcurement;
using LivingAzeroth::Economy::ProfileCommerce::CalculateAuctionCutForBuyout;
using LivingAzeroth::Economy::ProfileCommerce::PlanSettlement;
using LivingAzeroth::Economy::ProfileCommerce::SettlementActionKind;
using LivingAzeroth::Economy::ProfileCommerce::SettlementObservation;
using LivingAzeroth::Economy::ProfileCommerce::SettlementOutcome;

constexpr char MutationConfirmation[] = "I_ACCEPT_ATOMIC_GUILD_PROCUREMENT_V2";
constexpr char OwnedSystemMutationConfirmation[] =
    "I_ACCEPT_BOUNDED_OWNED_SYSTEM_PURCHASES_V1";
constexpr std::string_view DestinationActionType = "auction_buyout";
constexpr std::string_view CatalogSha256 =
    LivingAzeroth::Economy::FullPriceCatalogSha256;

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

constexpr char PurchaseInsertSql[] =
    "INSERT INTO la_guild_procurement_purchase "
    "(action_key,auction_id,status,faction,buyer_guid,buyer_account,seller_guid,"
    "seller_account,house_id,item_guid,item_entry,item_count,buyout_copper,"
    "reference_lot_copper,ceiling_copper,deposit_copper,cut_copper,"
    "wallet_before_copper,dispatched_at_unix,dispatch_evidence_sha256) VALUES "
    "('{}',{},'dispatched','{}',{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},'{}')";
constexpr char FactionUsageSql[] =
    "INSERT INTO la_guild_procurement_faction_daily "
    "(faction,utc_day,dispatch_count,dispatch_spend_copper) VALUES ('{}',{},1,{}) "
    "ON DUPLICATE KEY UPDATE dispatch_count=dispatch_count+1,"
    "dispatch_spend_copper=dispatch_spend_copper+{}";
constexpr char SellerUsageSql[] =
    "INSERT INTO la_guild_procurement_seller_daily "
    "(faction,seller_guid,utc_day,dispatch_count) VALUES ('{}',{},{},1) "
    "ON DUPLICATE KEY UPDATE dispatch_count=dispatch_count+1";
constexpr char SettlementUpdateSql[] =
    "UPDATE la_guild_procurement_purchase SET status='{}',buyer_debit_copper={},"
    "seller_mail_copper={},settlement_reason='{}',finalized_at_unix={},"
    "settlement_evidence_sha256='{}' WHERE action_key='{}' AND status='dispatched'";

static_assert(SqlPlaceholderCount(PurchaseInsertSql) == 19);
static_assert(SqlPlaceholderCount(FactionUsageSql) == 4);
static_assert(SqlPlaceholderCount(SellerUsageSql) == 3);
static_assert(SqlPlaceholderCount(SettlementUpdateSql) == 7);

struct RuntimeConfig
{
    bool MasterEnabled = false;
    bool OtherWritersCompatible = false;
    Procurement::Policy ObservePolicy;
    Procurement::ExecutionPolicy ExecutePolicy;
    std::uint32_t CycleMs = 60 * IN_MILLISECONDS;
    std::uint32_t AllianceBuyerGuid = 0;
    std::uint32_t HordeBuyerGuid = 0;
    std::string EnvironmentId;
    std::uint32_t RealmId = 0;
    std::string PolicyVersion;
    std::string ConfiguredCatalogSha256;
    std::unordered_set<std::uint32_t> SystemSellerGuids;
};

struct BuyerIdentity
{
    std::uint32_t Guid = 0;
    std::uint32_t Account = 0;
    Procurement::Faction Faction = Procurement::Faction::Other;
    bool Valid = false;
};

struct DailyUsage
{
    std::uint32_t Count = 0;
    std::uint64_t Spend = 0;
};

struct CandidateRuntime
{
    Procurement::Candidate Candidate;
    std::uint32_t SellerAccount = 0;
    std::string SellerName;
    std::string ItemName;
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
                "Guild procurement executor ignored invalid system GUID token '{}'.", token);
        }
    }
    return result;
}

char const* FactionName(Procurement::Faction faction)
{
    return faction == Procurement::Faction::Alliance ? "alliance" :
        faction == Procurement::Faction::Horde ? "horde" : "unsupported";
}

char const* ExecutionReasonName(Procurement::ExecutionReason reason)
{
    switch (reason)
    {
        case Procurement::ExecutionReason::Approved: return "approved";
        case Procurement::ExecutionReason::FeatureDisabled: return "feature_disabled";
        case Procurement::ExecutionReason::MutationModeDisabled: return "mutation_mode_disabled";
        case Procurement::ExecutionReason::ConfirmationMissing: return "confirmation_missing";
        case Procurement::ExecutionReason::SchemaUnavailable: return "schema_unavailable";
        case Procurement::ExecutionReason::CatalogHashMismatch: return "catalog_hash_mismatch";
        case Procurement::ExecutionReason::InvalidLimits: return "invalid_limits";
        case Procurement::ExecutionReason::ProposalRejected: return "proposal_rejected";
        case Procurement::ExecutionReason::UnsupportedFaction: return "unsupported_faction";
        case Procurement::ExecutionReason::BuyerNotConfigured: return "buyer_not_configured";
        case Procurement::ExecutionReason::BuyerNotOwnedAi: return "buyer_not_owned_ai";
        case Procurement::ExecutionReason::BuyerOffline: return "buyer_offline";
        case Procurement::ExecutionReason::BuyerNotAtAuctioneer: return "buyer_not_at_auctioneer";
        case Procurement::ExecutionReason::SelfTrade: return "self_trade";
        case Procurement::ExecutionReason::InvalidAuction: return "invalid_auction";
        case Procurement::ExecutionReason::BidPresent: return "bid_present";
        case Procurement::ExecutionReason::AuctionAlreadyClaimed: return "auction_already_claimed";
        case Procurement::ExecutionReason::BootMutationCap: return "boot_mutation_cap";
        case Procurement::ExecutionReason::FactionCycleCap: return "faction_cycle_cap";
        case Procurement::ExecutionReason::FactionDailyPurchaseCap: return "faction_daily_count_cap";
        case Procurement::ExecutionReason::FactionDailySpendCap: return "faction_daily_spend_cap";
        case Procurement::ExecutionReason::SellerDailyPurchaseCap: return "seller_daily_count_cap";
        case Procurement::ExecutionReason::OwnedSystemSellerDisabled: return "owned_system_seller_disabled";
        case Procurement::ExecutionReason::OwnedSystemConfirmationMissing: return "owned_system_confirmation_missing";
        case Procurement::ExecutionReason::OwnedSystemSellerNotAllowlisted: return "owned_system_seller_not_allowlisted";
        case Procurement::ExecutionReason::OwnedSystemDailyPurchaseCap: return "owned_system_daily_count_cap";
        case Procurement::ExecutionReason::InsufficientFunds: return "insufficient_funds";
        case Procurement::ExecutionReason::ReserveViolation: return "reserve_violation";
    }
    return "unknown";
}

std::string EvidenceHash(std::string const& payload)
{
    return ByteArrayToHexStr(Acore::Crypto::SHA256::GetDigestOf(payload));
}

bool ValidAuthorityToken(std::string_view token, std::size_t minimum, std::size_t maximum)
{
    if (token.size() < minimum || token.size() > maximum)
        return false;
    return std::all_of(token.begin(), token.end(), [](unsigned char character)
    {
        return std::isalnum(character) != 0 || character == '-' || character == '_' ||
            character == '.';
    });
}

struct DestinationReceiptRead
{
    LivingAzeroth::Economy::DestinationReceipt::State State =
        LivingAzeroth::Economy::DestinationReceipt::State::Invalid;
    std::string ResultSha256;
};

std::string BuildActionId(RuntimeConfig const& config, AuctionEntry const* auction)
{
    std::string const identity = config.EnvironmentId + "|" +
        std::to_string(config.RealmId) + "|" + std::string(DestinationActionType) + "|" +
        std::to_string(auction->Id) + "|" + std::to_string(auction->item_guid.GetCounter()) +
        "|" + std::to_string(auction->owner.GetCounter());
    return "outlands:" + config.EnvironmentId + ":" + std::to_string(config.RealmId) +
        ":auction:" + std::to_string(auction->Id) + ":" + EvidenceHash(identity).substr(0, 16);
}

std::string BuildActionPayload(RuntimeConfig const& config, std::string const& actionId,
    BuyerIdentity const& buyer, AuctionEntry const* auction,
    Procurement::Decision const& proposal, std::uint32_t sellerAccount)
{
    return config.EnvironmentId + "|" + std::to_string(config.RealmId) + "|" +
        std::string(DestinationActionType) + "|" + actionId + "|" + config.PolicyVersion +
        "|" + config.ConfiguredCatalogSha256 + "|" + std::to_string(auction->Id) + "|" +
        std::to_string(static_cast<std::uint32_t>(auction->houseId)) + "|" +
        std::to_string(buyer.Guid) + "|" + std::to_string(buyer.Account) + "|" +
        std::to_string(auction->owner.GetCounter()) + "|" + std::to_string(sellerAccount) +
        "|" + std::to_string(auction->item_guid.GetCounter()) + "|" +
        std::to_string(auction->item_template) + "|" + std::to_string(auction->itemCount) +
        "|" + std::to_string(auction->buyout) + "|" +
        std::to_string(proposal.ReferenceLotPrice) + "|" +
        std::to_string(proposal.CeilingPrice) + "|" + std::to_string(auction->deposit);
}

DestinationReceiptRead ReadDestinationReceipt(RuntimeConfig const& config,
    std::string const& actionId, std::string const& payloadSha256,
    AuctionEntry const* auction, BuyerIdentity const& buyer)
{
    DestinationReceiptRead read;
    QueryResult result = CharacterDatabase.Query(
        "SELECT COUNT(*),COALESCE(MAX(action_type),''),COALESCE(MAX(payload_sha256),''),"
        "COALESCE(MAX(result_sha256),''),COALESCE(MAX(environment_id='{}' AND realm_id={} "
        "AND policy_version='{}' AND auction_id={} AND buyer_guid={}),0) "
        "FROM la_outlands_processed_action WHERE action_id='{}'",
        config.EnvironmentId, config.RealmId, config.PolicyVersion, auction->Id, buyer.Guid,
        actionId);
    if (!result)
        return read;
    Field* fields = result->Fetch();
    LivingAzeroth::Economy::DestinationReceipt::Observation observed;
    observed.QuerySucceeded = true;
    observed.Exists = fields[0].Get<std::uint32_t>() == 1;
    observed.StoredActionType = fields[1].Get<std::string>();
    observed.StoredPayloadSha256 = fields[2].Get<std::string>();
    observed.ExpectedActionType = DestinationActionType;
    observed.ExpectedPayloadSha256 = payloadSha256;
    observed.IdentityMatches = fields[4].Get<std::uint32_t>() == 1;
    read.State = LivingAzeroth::Economy::DestinationReceipt::Classify(observed);
    read.ResultSha256 = fields[3].Get<std::string>();
    return read;
}

class LivingAzerothGuildProcurementExecutor final : public WorldScript
{
public:
    LivingAzerothGuildProcurementExecutor()
        : WorldScript("LivingAzerothGuildProcurementExecutor",
            { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED,
              WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE })
    {
    }

    void OnAfterConfigLoad(bool reload) override
    {
        if (reload)
        {
            LoadConfig(true);
            _allianceBuyer = ValidateBuyer(_config.AllianceBuyerGuid,
                Procurement::Faction::Alliance);
            _hordeBuyer = ValidateBuyer(_config.HordeBuyerGuid,
                Procurement::Faction::Horde);
            RefreshRuntimeBlock();
        }
    }

    void OnBeforeWorldInitialized() override
    {
        LoadConfig(false);
    }

    void OnStartup() override
    {
        _elapsedMs = 0;
        _mutationsThisBoot = 0;
        _ownedAiAccounts.clear();
        _claimedAuctionIds.clear();
        _schemaReady = false;
        _catalogHashReady = false;
        _priceIndexReady = false;
        if (!_config.ExecutePolicy.Enabled)
        {
            _runtimeBlocked = true;
            LOG_INFO("living_azeroth.economy", "Guild procurement executor disabled.");
            return;
        }
        _priceIndexReady = LivingAzeroth::Economy::FullPriceIndex::Instance().EnsureLoaded();
        LoadOwnedAiAccounts();
        LoadSchemaIdentity();
        LoadClaims();
        _allianceBuyer = ValidateBuyer(_config.AllianceBuyerGuid,
            Procurement::Faction::Alliance);
        _hordeBuyer = ValidateBuyer(_config.HordeBuyerGuid,
            Procurement::Faction::Horde);
        RefreshRuntimeBlock();
        LOG_INFO("living_azeroth.economy",
            "Guild procurement executor startup enabled={} mutation={} blocked={} schema={} "
            "catalog={} writersCompatible={} aiAccounts={} claims={} allianceBuyer={} "
            "hordeBuyer={} mutationPathCompiled=true.",
            _config.ExecutePolicy.Enabled, _config.ExecutePolicy.MutationMode,
            _runtimeBlocked, _schemaReady, _catalogHashReady,
            _config.OtherWritersCompatible, _ownedAiAccounts.size(),
            _claimedAuctionIds.size(), _allianceBuyer.Valid, _hordeBuyer.Valid);
    }

    void OnUpdate(std::uint32_t diff) override
    {
        _elapsedMs += diff;
        if (_elapsedMs < _config.CycleMs)
            return;
        _elapsedMs = 0;
        if (_schemaReady)
            ReconcileSettlements();
        if (_runtimeBlocked)
            return;
        for (std::uint32_t slot = 0;
            slot < _config.ExecutePolicy.MaxPurchasesPerFactionPerCycle; ++slot)
        {
            RunFaction(_allianceBuyer, AuctionHouseId::Alliance, slot);
            RunFaction(_hordeBuyer, AuctionHouseId::Horde, slot);
        }
    }

private:
    void LoadConfig(bool reload)
    {
        _runtimeBlocked = true;
        _config.MasterEnabled = sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.Enable", false);
        auto& execute = _config.ExecutePolicy;
        execute.Enabled = _config.MasterEnabled && sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.GuildProcurement.Execution.Enable", false);
        execute.MutationMode = sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.GuildProcurement.Execution.MutationMode", false);
        execute.ConfirmationValid = sConfigMgr->GetOption<std::string>(
            "LivingAzeroth.Economy.GuildProcurement.Execution.Confirmation", "") ==
            MutationConfirmation;
        execute.AllowOwnedSystemSellerAuctions = sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.GuildProcurement.AllowOwnedSystemSellerAuctions", false);
        execute.OwnedSystemConfirmationValid = sConfigMgr->GetOption<std::string>(
            "LivingAzeroth.Economy.GuildProcurement.Execution.OwnedSystemConfirmation", "") ==
            OwnedSystemMutationConfirmation;
        execute.MaxPurchasesPerFactionPerDay = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.GuildProcurement.MaxPurchasesPerFactionPerDay", 5);
        execute.MaxSpendPerFactionPerDay = sConfigMgr->GetOption<std::uint64_t>(
            "LivingAzeroth.Economy.GuildProcurement.MaxSpendCopperPerFactionPerDay", 50000);
        execute.MaxPurchasesPerSellerPerDay = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.GuildProcurement.MaxPurchasesPerSellerPerDay", 2);
        execute.MaxOwnedSystemPurchasesPerFactionPerDay = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.GuildProcurement.MaxOwnedSystemPurchasesPerFactionPerDay", 20);
        execute.MaxPurchasesPerFactionPerCycle = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.GuildProcurement.Execution.MaxPurchasesPerFactionPerCycle", 1);
        execute.MaxMutationsPerBoot = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.GuildProcurement.Execution.MaxMutationsPerBoot", 2);
        execute.MinimumBuyerReserve = sConfigMgr->GetOption<std::uint64_t>(
            "LivingAzeroth.Economy.GuildProcurement.Execution.MinimumBuyerReserveCopper", 1000);
        execute.SchemaReady = _schemaReady;
        _config.EnvironmentId = sConfigMgr->GetOption<std::string>(
            "LivingAzeroth.Economy.GuildProcurement.Execution.EnvironmentId", "");
        _config.RealmId = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.GuildProcurement.Execution.RealmId", 0);
        _config.PolicyVersion = sConfigMgr->GetOption<std::string>(
            "LivingAzeroth.Economy.GuildProcurement.Execution.PolicyVersion", "");
        _config.ConfiguredCatalogSha256 = sConfigMgr->GetOption<std::string>(
            "LivingAzeroth.Economy.GuildProcurement.Execution.CatalogSha256", "");
        execute.CatalogHashValid = _priceIndexReady && _catalogHashReady &&
            _config.ConfiguredCatalogSha256 == CatalogSha256;
        _config.AllianceBuyerGuid = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.GuildProcurement.Execution.AllianceBuyerGuid", 0);
        _config.HordeBuyerGuid = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.GuildProcurement.Execution.HordeBuyerGuid", 0);
        std::uint32_t const cycleSeconds = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.GuildProcurement.CycleSeconds", 60);
        _cycleConfigValid = cycleSeconds >= 30 && cycleSeconds <= 3600;
        _config.CycleMs = std::clamp(cycleSeconds, std::uint32_t(30),
            std::uint32_t(3600)) * IN_MILLISECONDS;
        _config.SystemSellerGuids = ParseGuidSet(sConfigMgr->GetOption<std::string>(
            "LivingAzeroth.Economy.GuildProcurement.OwnedSystemSellerGuids", "900001"));
        _authorityConfigValid = ValidAuthorityToken(_config.EnvironmentId, 3, 32) &&
            _config.RealmId > 0 && ValidAuthorityToken(_config.PolicyVersion, 3, 64);

        bool const staticSellerEnabled = sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.StaticSeller.Enable", false);
        std::uint32_t const staticSellerTier = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.StaticSeller.ListingTier", 0);
        std::uint32_t const staticSellerMaxLive = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.StaticSeller.MaxLiveListings", 0);
        LivingAzeroth::Economy::WriterCompatibility::StaticSellerObservation
            staticSellerObserved;
        staticSellerObserved.Enabled = staticSellerEnabled;
        staticSellerObserved.ProcurementMayBuyOwnedSystem =
            execute.AllowOwnedSystemSellerAuctions;
        staticSellerObserved.SystemSellerCount =
            static_cast<std::uint32_t>(_config.SystemSellerGuids.size());
        staticSellerObserved.LockedSystemSellerPresent =
            _config.SystemSellerGuids.contains(900001);
        staticSellerObserved.Account = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.StaticSeller.Account", 0);
        staticSellerObserved.Guid = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.StaticSeller.GUID", 0);
        staticSellerObserved.DurationHours = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.StaticSeller.DurationHours", 0);
        staticSellerObserved.ListingTier = staticSellerTier;
        staticSellerObserved.MaxLiveListings = staticSellerMaxLive;
        staticSellerObserved.CatalogMatches = sConfigMgr->GetOption<std::string>(
            "LivingAzeroth.Economy.StaticSeller.CatalogSha256", "") == CatalogSha256;
        staticSellerObserved.ReclaimExpiredMail = sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.StaticSeller.ReclaimExpiredMail.Enable", false);
        staticSellerObserved.ReclaimMaxPerRun = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.StaticSeller.ReclaimExpiredMail.MaxPerRun", 0);
        staticSellerObserved.RelistOnExpire = sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.StaticSeller.RelistOnExpire.Enable", false);
        bool const staticSellerCompatible =
            LivingAzeroth::Economy::WriterCompatibility::Compatible(staticSellerObserved);
        bool const profileEnabled = sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.ProfileCommerce.Enable", false);
        bool const profileObserveOnly = sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.ProfileCommerce.ObserveOnly", true);
        bool const profileSellerDisabled = !sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.ProfileCommerce.Seller.Enable", false);
        bool const profileBuyerDisabled = !sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.ProfileCommerce.Buyer.Enable", false);
        bool const profileHasNoMutation = !profileEnabled || profileObserveOnly ||
            (profileSellerDisabled && profileBuyerDisabled);
        bool const atomicSellerOnly = profileEnabled && !profileObserveOnly &&
            !profileSellerDisabled && profileBuyerDisabled &&
            sConfigMgr->GetOption<bool>(
                "LivingAzeroth.Economy.ProfileCommerce.Seller.AtomicReceipt.Enable", false) &&
            sConfigMgr->GetOption<std::string>(
                "LivingAzeroth.Economy.ProfileCommerce.Seller.AtomicReceipt.Confirmation", "") ==
                "I_ACCEPT_ATOMIC_AI_LISTINGS_V1" &&
            ValidAuthorityToken(sConfigMgr->GetOption<std::string>(
                "LivingAzeroth.Economy.ProfileCommerce.Seller.EnvironmentId", ""), 3, 32) &&
            sConfigMgr->GetOption<std::uint32_t>(
                "LivingAzeroth.Economy.ProfileCommerce.Seller.RealmId", 0) > 0 &&
            ValidAuthorityToken(sConfigMgr->GetOption<std::string>(
                "LivingAzeroth.Economy.ProfileCommerce.Seller.PolicyVersion", ""), 3, 64) &&
            sConfigMgr->GetOption<std::uint32_t>(
                "LivingAzeroth.Economy.ProfileCommerce.MaxMutationsPerBoot", 0) > 0 &&
            sConfigMgr->GetOption<std::string>(
                "LivingAzeroth.Economy.ProfileCommerce.CatalogSha256", "") == CatalogSha256;
        _config.OtherWritersCompatible = staticSellerCompatible &&
            (profileHasNoMutation || atomicSellerOnly);

        auto& observe = _config.ObservePolicy;
        observe.Enabled = execute.Enabled;
        observe.ObserveOnly = true;
        observe.ConfirmationValid = true;
        observe.RequireCatalogApproval = true;
        observe.AllowOwnedSystemSellerAuctions = execute.AllowOwnedSystemSellerAuctions;
        observe.CeilingPercent = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.GuildProcurement.CeilingPercent", 110);
        observe.MaxPurchasesPerFactionPerDay = execute.MaxPurchasesPerFactionPerDay;
        observe.MaxSpendPerFactionPerDay = execute.MaxSpendPerFactionPerDay;
        observe.MaxPurchasesPerSellerPerDay = execute.MaxPurchasesPerSellerPerDay;
        observe.MaxOwnedSystemPurchasesPerFactionPerDay =
            execute.MaxOwnedSystemPurchasesPerFactionPerDay;
        observe.MaxProposalsPerFactionPerCycle =
            execute.MaxPurchasesPerFactionPerCycle;
        RefreshRuntimeBlock();
        if (reload)
            _elapsedMs = 0;
        LOG_INFO("living_azeroth.economy",
            "Guild procurement executor config {} enabled={} mutation={} confirmation={} "
            "cycleValid={} limitsValid={} catalog={} buyers={}/{} writersCompatible={} blocked={}.",
            reload ? "reloaded" : "initialized", execute.Enabled,
            execute.MutationMode, execute.ConfirmationValid, _cycleConfigValid,
            Procurement::ValidExecutionLimits(execute), execute.CatalogHashValid,
            _config.AllianceBuyerGuid, _config.HordeBuyerGuid,
            _config.OtherWritersCompatible, _runtimeBlocked);
    }

    void RefreshRuntimeBlock()
    {
        _config.ExecutePolicy.SchemaReady = _schemaReady;
        _config.ExecutePolicy.CatalogHashValid = _priceIndexReady && _catalogHashReady &&
            _config.ConfiguredCatalogSha256 == CatalogSha256;
        _runtimeBlocked = !_config.MasterEnabled || !_config.ExecutePolicy.Enabled ||
            !_config.ExecutePolicy.MutationMode ||
            !_config.ExecutePolicy.ConfirmationValid ||
            (_config.ExecutePolicy.AllowOwnedSystemSellerAuctions &&
                (!_config.ExecutePolicy.OwnedSystemConfirmationValid ||
                 _config.SystemSellerGuids.size() != 1 ||
                 !_config.SystemSellerGuids.contains(900001))) || !_schemaReady ||
            !_config.ExecutePolicy.CatalogHashValid || !_cycleConfigValid ||
            !Procurement::ValidExecutionLimits(_config.ExecutePolicy) ||
            !_authorityConfigValid || !_config.OtherWritersCompatible || _ownedAiAccounts.empty() ||
            !_allianceBuyer.Valid || !_hordeBuyer.Valid;
    }

    void LoadOwnedAiAccounts()
    {
        QueryResult result = PlayerbotsDatabase.Query(
            "SELECT account_id FROM playerbots_account_type WHERE account_type IN (1,2) ORDER BY account_id");
        if (!result)
            return;
        do
        {
            _ownedAiAccounts.insert(result->Fetch()[0].Get<std::uint32_t>());
        } while (result->NextRow());
    }

    void LoadSchemaIdentity()
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT schema_version,catalog_sha256 FROM la_guild_procurement_schema WHERE singleton_id=1");
        if (!result)
            return;
        Field* fields = result->Fetch();
        _schemaReady = fields[0].Get<std::uint32_t>() == 5;
        _catalogHashReady = fields[1].Get<std::string>() == CatalogSha256;
    }

    void LoadClaims()
    {
        if (!_schemaReady)
            return;
        QueryResult result = CharacterDatabase.Query(
            "SELECT auction_id FROM la_guild_procurement_purchase ORDER BY auction_id");
        if (!result)
            return;
        do
        {
            _claimedAuctionIds.insert(result->Fetch()[0].Get<std::uint32_t>());
        } while (result->NextRow());
    }

    BuyerIdentity ValidateBuyer(std::uint32_t guid, Procurement::Faction faction) const
    {
        BuyerIdentity identity;
        identity.Guid = guid;
        identity.Faction = faction;
        if (guid == 0)
            return identity;
        ObjectGuid const objectGuid = ObjectGuid::Create<HighGuid::Player>(guid);
        CharacterCacheEntry const* cached = sCharacterCache->GetCharacterCacheByGuid(objectGuid);
        TeamId const expected = faction == Procurement::Faction::Alliance ?
            TEAM_ALLIANCE : TEAM_HORDE;
        if (!cached || cached->AccountId == 0 ||
            !_ownedAiAccounts.contains(cached->AccountId) ||
            Player::TeamIdForRace(cached->Race) != expected)
            return identity;
        identity.Account = cached->AccountId;
        identity.Valid = true;
        return identity;
    }

    static Player* FindLoadedBuyer(std::uint32_t guid)
    {
        for (auto const& [key, bot] : sRandomPlayerbotMgr.GetAllBots())
        {
            (void)key;
            if (bot && bot->GetGUID().GetCounter() == guid)
                return bot;
        }
        return nullptr;
    }

    std::vector<Procurement::BuyerSelectionCandidate> CollectBuyerCandidates(
        Procurement::Faction faction) const
    {
        std::vector<Procurement::BuyerSelectionCandidate> candidates;
        TeamId const expected = faction == Procurement::Faction::Alliance ?
            TEAM_ALLIANCE : TEAM_HORDE;
        for (auto const& [key, bot] : sRandomPlayerbotMgr.GetAllBots())
        {
            (void)key;
            if (!bot)
                continue;
            CharacterCacheEntry const* cached =
                sCharacterCache->GetCharacterCacheByGuid(bot->GetGUID());
            Procurement::BuyerSelectionCandidate candidate;
            candidate.Guid = bot->GetGUID().GetCounter();
            candidate.Account = cached ? cached->AccountId : 0;
            candidate.CharacterFaction = cached &&
                Player::TeamIdForRace(cached->Race) == expected ? faction :
                Procurement::Faction::Other;
            candidate.Money = bot->GetMoney();
            candidate.OwnedAi = candidate.Account != 0 &&
                _ownedAiAccounts.contains(candidate.Account);
            candidate.Loaded = true;
            candidate.SessionReady = bot->GetSession() != nullptr;
            candidate.InWorld = bot->IsInWorld();
            candidate.Teleporting = bot->IsBeingTeleported();
            candidate.RandomBot = sRandomPlayerbotMgr.IsRandomBot(bot);
            candidates.push_back(candidate);
        }
        return candidates;
    }

    static bool BuyerAvailable(Player* buyer)
    {
        return buyer && buyer->GetSession() && buyer->IsInWorld() &&
            !buyer->IsBeingTeleported() && sRandomPlayerbotMgr.IsRandomBot(buyer);
    }

    static Creature* FindAuctioneer(Player* buyer, PlayerbotAI* ai)
    {
        if (!buyer)
            return nullptr;

        constexpr std::array<std::uint32_t, 6> ServiceAuctioneerEntries = {
            8719, 8670, 15659, 8724, 8673, 9856
        };
        for (std::uint32_t entry : ServiceAuctioneerEntries)
        {
            if (Creature* nearby = buyer->FindNearestCreature(entry, 20.0f))
            {
                if (Creature* auctioneer = buyer->GetNPCIfCanInteractWith(
                        nearby->GetGUID(), UNIT_NPC_FLAG_AUCTIONEER))
                    return auctioneer;
                LOG_INFO("living_azeroth.economy",
                    "Guild procurement auctioneer rejected buyerGuid={} entry={} map={} "
                    "distance={} buyerAlive={} buyerFlight={} npcAlive={} npcFlag={} reaction={}.",
                    buyer->GetGUID().GetCounter(), entry, buyer->GetMapId(),
                    buyer->GetDistance(nearby), buyer->IsAlive(), buyer->IsInFlight(),
                    nearby->IsAlive(), nearby->HasNpcFlag(UNIT_NPC_FLAG_AUCTIONEER),
                    static_cast<std::int32_t>(nearby->GetReactionTo(buyer)));
            }
        }

        if (!ai)
            return nullptr;
        GuidVector const nearby = ai->GetAiObjectContext()->GetValue<GuidVector>(
            "nearest npcs")->Get();
        for (ObjectGuid const& guid : nearby)
        {
            if (Creature* auctioneer = buyer->GetNPCIfCanInteractWith(
                    guid, UNIT_NPC_FLAG_AUCTIONEER))
                return auctioneer;
        }
        LOG_INFO("living_azeroth.economy",
            "Guild procurement auctioneer missing buyerGuid={} map={} x={} y={} z={} "
            "alive={} flight={} nearbyNpcGuids={}.",
            buyer->GetGUID().GetCounter(), buyer->GetMapId(), buyer->GetPositionX(),
            buyer->GetPositionY(), buyer->GetPositionZ(), buyer->IsAlive(),
            buyer->IsInFlight(), nearby.size());
        return nullptr;
    }

    Procurement::OwnerKind ClassifyOwner(ObjectGuid owner, std::uint32_t& account,
        std::string& name) const
    {
        std::uint32_t const guid = owner.GetCounter();
        CharacterCacheEntry const* cached = sCharacterCache->GetCharacterCacheByGuid(owner);
        account = cached ? cached->AccountId : 0;
        name = cached ? cached->Name : "unknown";
        if (_config.SystemSellerGuids.contains(guid))
            return Procurement::OwnerKind::OwnedSystem;
        if (account != 0 && _ownedAiAccounts.contains(account))
            return Procurement::OwnerKind::OwnedAi;
        return cached ? Procurement::OwnerKind::Human : Procurement::OwnerKind::Unknown;
    }

    static bool SafeItem(AuctionEntry const* auction, Item const* item,
        ItemTemplate const* itemTemplate)
    {
        if (!auction || !item || !itemTemplate || item->GetGUID() != auction->item_guid ||
            item->GetEntry() != auction->item_template || item->GetCount() != auction->itemCount)
            return false;
        if (itemTemplate->HasFlag(ITEM_FLAG_CONJURED) ||
            itemTemplate->HasFlag(ITEM_FLAG_IS_BOUND_TO_ACCOUNT) ||
            (itemTemplate->Bonding != NO_BIND &&
                itemTemplate->Bonding != BIND_WHEN_EQUIPPED &&
                itemTemplate->Bonding != BIND_WHEN_USE) ||
            itemTemplate->Duration != 0 || itemTemplate->StartQuest != 0 ||
            itemTemplate->MinMoneyLoot != 0 || itemTemplate->MaxMoneyLoot != 0 ||
            itemTemplate->GetMaxStackSize() < auction->itemCount)
            return false;
        return true;
    }

    DailyUsage LoadFactionUsage(Procurement::Faction faction, std::uint64_t utcDay) const
    {
        DailyUsage usage;
        QueryResult result = CharacterDatabase.Query(
            "SELECT dispatch_count,dispatch_spend_copper "
            "FROM la_guild_procurement_faction_daily WHERE faction='{}' AND utc_day={}",
            FactionName(faction), utcDay);
        if (result)
        {
            usage.Count = result->Fetch()[0].Get<std::uint32_t>();
            usage.Spend = result->Fetch()[1].Get<std::uint64_t>();
        }
        return usage;
    }

    std::unordered_map<std::uint32_t, std::uint32_t> LoadSellerUsage(
        Procurement::Faction faction, std::uint64_t utcDay) const
    {
        std::unordered_map<std::uint32_t, std::uint32_t> usage;
        QueryResult result = CharacterDatabase.Query(
            "SELECT seller_guid,dispatch_count FROM la_guild_procurement_seller_daily "
            "WHERE faction='{}' AND utc_day={} ORDER BY seller_guid",
            FactionName(faction), utcDay);
        if (!result)
            return usage;
        do
        {
            Field* fields = result->Fetch();
            usage[fields[0].Get<std::uint32_t>()] = fields[1].Get<std::uint32_t>();
        } while (result->NextRow());
        return usage;
    }

    void AppendDispatch(CharacterDatabaseTransaction& transaction,
        BuyerIdentity const& identity, AuctionEntry const* auction,
        Procurement::Decision const& proposal, std::uint32_t sellerAccount,
        std::uint64_t walletBefore, std::uint64_t cut, std::string const& actionId,
        std::string const& payloadSha256)
    {
        std::string const faction = FactionName(identity.Faction);
        std::uint64_t const now = GameTime::GetGameTime().count();
        std::uint64_t const utcDay = now / DAY;
        std::string const evidence = EvidenceHash(
            actionId + "|dispatched|" + _config.PolicyVersion + "|" + payloadSha256 + "|" +
            std::to_string(identity.Guid) + "|" + std::to_string(identity.Account) + "|" +
            std::to_string(auction->owner.GetCounter()) + "|" +
            std::to_string(auction->item_guid.GetCounter()) + "|" +
            std::to_string(auction->item_template) + "|" +
            std::to_string(auction->itemCount) + "|" + std::to_string(auction->buyout) +
            "|" + std::to_string(proposal.ReferenceLotPrice) + "|" +
            std::to_string(proposal.CeilingPrice) + "|" + std::to_string(now));

        transaction->Append(PurchaseInsertSql, actionId, auction->Id, faction,
            identity.Guid, identity.Account, auction->owner.GetCounter(), sellerAccount,
            static_cast<std::uint32_t>(auction->houseId), auction->item_guid.GetCounter(),
            auction->item_template, auction->itemCount, auction->buyout,
            proposal.ReferenceLotPrice, proposal.CeilingPrice, auction->deposit, cut,
            walletBefore, now, evidence);
        transaction->Append(FactionUsageSql, faction, utcDay, auction->buyout,
            auction->buyout);
        transaction->Append(SellerUsageSql, faction, auction->owner.GetCounter(), utcDay);
    }

    void RunFaction(BuyerIdentity const& identity, AuctionHouseId expectedHouse,
        std::uint32_t purchaseSlot)
    {
        AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(expectedHouse);
        if (!house)
        {
            LOG_ERROR("living_azeroth.economy",
                "Guild procurement executor blocked faction={} buyerGuid={} reason=wrong_house.",
                FactionName(identity.Faction), identity.Guid);
            return;
        }

        std::uint64_t const utcDay =
            static_cast<std::uint64_t>(GameTime::GetGameTime().count()) / DAY;
        DailyUsage const factionUsage = LoadFactionUsage(identity.Faction, utcDay);
        auto sellerUsage = LoadSellerUsage(identity.Faction, utcDay);
        std::uint32_t ownedSystemUsage = 0;
        for (std::uint32_t guid : _config.SystemSellerGuids)
            ownedSystemUsage += sellerUsage[guid];
        std::vector<CandidateRuntime> runtimeCandidates;
        std::vector<Procurement::Candidate> candidates;
        std::array<std::uint32_t,
            static_cast<std::size_t>(Procurement::Reason::FactionCycleCap) + 1> reasons{};
        for (auto const& [id, auction] : house->GetAuctions())
        {
            (void)id;
            if (!auction)
                continue;
            CandidateRuntime candidate;
            candidate.Candidate.AuctionId = auction->Id;
            Procurement::Observation& observed = candidate.Candidate.Observed;
            observed.MarketFaction = identity.Faction;
            observed.SellerKind = ClassifyOwner(auction->owner,
                candidate.SellerAccount, candidate.SellerName);
            observed.SellerSystemAllowlisted = observed.SellerKind ==
                Procurement::OwnerKind::OwnedSystem &&
                _config.SystemSellerGuids.contains(auction->owner.GetCounter());
            observed.CatalogApproved = LivingAzeroth::Economy::FullPriceIndex::Instance().Approved(
                auction->item_template);
            ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(
                auction->item_template);
            candidate.ItemName = itemTemplate ? itemTemplate->Name1 : "unknown";
            observed.SafeItem = SafeItem(auction,
                sAuctionMgr->GetAItem(auction->item_guid), itemTemplate);
            observed.HasBid = static_cast<bool>(auction->bidder) || auction->bid != 0;
            observed.DuplicateAuction = _claimedAuctionIds.contains(auction->Id);
            observed.BuyerAccount = identity.Account;
            observed.SellerAccount = candidate.SellerAccount;
            observed.SellerGuid = auction->owner.GetCounter();
            observed.ItemCount = auction->itemCount;
            observed.FactionPurchasesToday = factionUsage.Count;
            observed.SellerPurchasesToday = sellerUsage[observed.SellerGuid];
            observed.OwnedSystemPurchasesToday = ownedSystemUsage;
            observed.FactionProposalsThisCycle = purchaseSlot;
            observed.ReferenceUnitPrice = LivingAzeroth::Economy::FullPriceIndex::Instance().ProcurementUnitPrice(
                auction->item_template);
            observed.Buyout = auction->buyout;
            observed.FactionSpendToday = factionUsage.Spend;
            Procurement::Decision const planned = Procurement::Plan(
                _config.ObservePolicy, observed);
            ++reasons[static_cast<std::size_t>(planned.Why)];
            candidates.push_back(candidate.Candidate);
            runtimeCandidates.push_back(std::move(candidate));
        }

        Procurement::Selection const selected = Procurement::Select(
            _config.ObservePolicy, candidates);
        if (!selected.Found)
        {
            LOG_INFO("living_azeroth.economy",
                "Guild procurement executor cycle faction={} buyerGuid={} scanned={} "
                "proposal=false approved={} selfTrade={} catalog={} unsafe={} bid={} "
                "duplicate={} missingPrice={} price={} factionCount={} factionSpend={} "
                "sellerCap={} mutation=false.", FactionName(identity.Faction),
                identity.Guid, candidates.size(),
                reasons[static_cast<std::size_t>(Procurement::Reason::ObserveOnlyApproved)],
                reasons[static_cast<std::size_t>(Procurement::Reason::SelfTrade)],
                reasons[static_cast<std::size_t>(Procurement::Reason::CatalogRejected)],
                reasons[static_cast<std::size_t>(Procurement::Reason::UnsafeItem)],
                reasons[static_cast<std::size_t>(Procurement::Reason::BidPresent)],
                reasons[static_cast<std::size_t>(Procurement::Reason::DuplicateAuction)],
                reasons[static_cast<std::size_t>(Procurement::Reason::MissingReferencePrice)],
                reasons[static_cast<std::size_t>(Procurement::Reason::PurchasePriceCeiling)],
                reasons[static_cast<std::size_t>(Procurement::Reason::FactionDailyPurchaseCap)],
                reasons[static_cast<std::size_t>(Procurement::Reason::FactionDailySpendCap)],
                reasons[static_cast<std::size_t>(Procurement::Reason::SellerDailyPurchaseCap)]);
            return;
        }

        auto runtime = std::find_if(runtimeCandidates.begin(), runtimeCandidates.end(),
            [&](CandidateRuntime const& row)
            {
                return row.Candidate.AuctionId == selected.AuctionId;
            });
        AuctionEntry* auction = house->GetAuction(selected.AuctionId);
        if (runtime == runtimeCandidates.end() || !auction || !auction->auctionHouseEntry)
            return;

        std::vector<Procurement::BuyerSelectionCandidate> buyerCandidates =
            CollectBuyerCandidates(identity.Faction);
        Procurement::BuyerSelection const buyerSelection = Procurement::SelectBuyer(
            identity.Guid, identity.Faction, runtime->SellerAccount, auction->buyout,
            _config.ExecutePolicy.MinimumBuyerReserve, buyerCandidates);
        if (!buyerSelection.Found)
        {
            LOG_INFO("living_azeroth.economy",
                "Guild procurement executor idle faction={} preferredBuyerGuid={} "
                "eligibleBuyer=false candidates={} auction={} mutation=false.",
                FactionName(identity.Faction), identity.Guid, buyerCandidates.size(),
                auction->Id);
            return;
        }
        BuyerIdentity selectedIdentity = ValidateBuyer(
            buyerSelection.Guid, identity.Faction);
        Player* buyer = FindLoadedBuyer(selectedIdentity.Guid);
        bool const online = BuyerAvailable(buyer);
        bool const backendHouseAuthorized = online && selectedIdentity.Valid;
        if (!backendHouseAuthorized || !buyer ||
            buyer->GetSession()->GetAccountId() != selectedIdentity.Account)
        {
            LOG_INFO("living_azeroth.economy",
                "Guild procurement executor idle faction={} preferredBuyerGuid={} "
                "selectedBuyerGuid={} reason=buyer_state_changed mutation=false.",
                FactionName(identity.Faction), identity.Guid, selectedIdentity.Guid);
            return;
        }
        bool const failover = selectedIdentity.Guid != identity.Guid;
        LOG_INFO("living_azeroth.economy",
            "Guild procurement buyer selected faction={} preferredBuyerGuid={} "
            "selectedBuyerGuid={} failover={} auction={}.", FactionName(identity.Faction),
            identity.Guid, selectedIdentity.Guid, failover, auction->Id);

        Procurement::ExecutionObservation executeObserved;
        executeObserved.ProposalApproved = selected.Planned.WouldPropose;
        executeObserved.MarketFaction = identity.Faction;
        executeObserved.BuyerConfigured = selectedIdentity.Guid != 0;
        executeObserved.BuyerOwnedAi = selectedIdentity.Valid;
        executeObserved.BuyerOnline = online;
        executeObserved.BuyerAtAuctioneer = backendHouseAuthorized;
        executeObserved.HasBid = static_cast<bool>(auction->bidder) || auction->bid != 0;
        executeObserved.AuctionClaimed = _claimedAuctionIds.contains(auction->Id);
        executeObserved.SellerKind = runtime->Candidate.Observed.SellerKind;
        executeObserved.SellerSystemAllowlisted =
            runtime->Candidate.Observed.SellerSystemAllowlisted;
        executeObserved.BuyerAccount = selectedIdentity.Account;
        executeObserved.SellerAccount = runtime->SellerAccount;
        executeObserved.SellerGuid = auction->owner.GetCounter();
        executeObserved.ItemCount = auction->itemCount;
        executeObserved.MutationsThisBoot = _mutationsThisBoot;
        executeObserved.PurchasesThisFactionCycle = purchaseSlot;
        executeObserved.FactionPurchasesToday = factionUsage.Count;
        executeObserved.SellerPurchasesToday = sellerUsage[executeObserved.SellerGuid];
        executeObserved.OwnedSystemPurchasesToday = ownedSystemUsage;
        executeObserved.FactionSpendToday = factionUsage.Spend;
        executeObserved.Buyout = auction->buyout;
        executeObserved.BuyerMoney = buyer->GetMoney();
        Procurement::ExecutionDecision const decision = Procurement::PlanExecution(
            _config.ExecutePolicy, executeObserved);
        LOG_INFO("living_azeroth.economy",
            "Guild procurement executor decision={} faction={} buyerGuid={} auction={} "
            "seller={} sellerGuid={} item={} itemEntry={} count={} buyout={} "
            "referenceLot={} ceiling={} wallet={} mutation={}.",
            ExecutionReasonName(decision.Why), FactionName(identity.Faction),
            selectedIdentity.Guid, auction->Id, runtime->SellerName,
            auction->owner.GetCounter(), runtime->ItemName, auction->item_template,
            auction->itemCount, auction->buyout, selected.Planned.ReferenceLotPrice,
            selected.Planned.CeilingPrice, buyer->GetMoney(), decision.MayMutate);
        if (!decision.MayMutate)
            return;
        if (!sScriptMgr->OnPlayerCanPlaceAuctionBid(buyer, auction))
        {
            LOG_ERROR("living_azeroth.economy",
                "Guild procurement executor blocked faction={} buyerGuid={} auction={} "
                "reason=script_restricted mutation=false.",
                FactionName(identity.Faction), selectedIdentity.Guid, auction->Id);
            return;
        }

        std::uint32_t const auctionId = auction->Id;
        std::uint32_t const buyout = auction->buyout;
        std::uint64_t const cut = CalculateAuctionCutForBuyout(
            buyout, auction->auctionHouseEntry->cutPercent,
            sWorld->getRate(RATE_AUCTION_CUT));
        std::string const actionId = BuildActionId(_config, auction);
        std::string const payload = BuildActionPayload(_config, actionId, selectedIdentity, auction,
            selected.Planned, runtime->SellerAccount);
        std::string const payloadSha256 = EvidenceHash(payload);
        DestinationReceiptRead existing = ReadDestinationReceipt(
            _config, actionId, payloadSha256, auction, selectedIdentity);
        if (existing.State == LivingAzeroth::Economy::DestinationReceipt::State::Applied)
        {
            _claimedAuctionIds.insert(auctionId);
            LOG_INFO("living_azeroth.economy",
                "Guild procurement duplicate suppressed actionId={} faction={} auction={} "
                "resultSha256={} receiptReadback=true mutation=false.", actionId,
                FactionName(identity.Faction), auctionId, existing.ResultSha256);
            return;
        }
        if (existing.State != LivingAzeroth::Economy::DestinationReceipt::State::Missing)
        {
            _runtimeBlocked = true;
            LOG_FATAL("living_azeroth.economy",
                "Guild procurement quarantined actionId={} faction={} auction={} "
                "receiptState={} mutation=false.", actionId, FactionName(identity.Faction),
                auctionId, static_cast<std::uint32_t>(existing.State));
            return;
        }

        if (buyer->HasUnitState(UNIT_STATE_DIED))
            buyer->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);

        std::uint64_t const now = GameTime::GetGameTime().count();
        std::uint32_t const walletBefore = buyer->GetMoney();
        ObjectGuid const bidderBefore = auction->bidder;
        std::uint32_t const bidBefore = auction->bid;
        ObjectGuid const itemGuid = auction->item_guid;
        std::string const resultSha256 = EvidenceHash(
            actionId + "|" + payloadSha256 + "|applied|" + std::to_string(now));

        CharacterDatabaseTransaction transaction = CharacterDatabase.BeginTransaction();
        AppendDispatch(transaction, selectedIdentity, auction, selected.Planned,
            runtime->SellerAccount, walletBefore, cut, actionId, payloadSha256);
        buyer->ModifyMoney(-static_cast<std::int32_t>(buyout));
        auction->bidder = buyer->GetGUID();
        auction->bid = buyout;
        sAuctionMgr->SendAuctionSalePendingMail(auction, transaction, true);
        sAuctionMgr->SendAuctionSuccessfulMail(auction, transaction, false, false, true);
        sAuctionMgr->SendAuctionWonMail(auction, transaction, false, false, true);
        auction->DeleteFromDB(transaction);
        // The procurement mutation changes the buyer's gold only. Keeping unrelated
        // dirty inventory state out of this synchronous transaction is required:
        // inventory statements are prepared only on asynchronous character DB
        // connections, while DirectCommitTransaction uses a synchronous connection.
        buyer->SaveGoldToDB(transaction);
        transaction->Append(
            "INSERT INTO la_outlands_processed_action "
            "(action_id,action_type,environment_id,realm_id,policy_version,payload_sha256,"
            "result_sha256,auction_id,buyer_guid,seller_guid,item_guid,item_entry,item_count,"
            "buyout_copper,applied_at_unix) VALUES ('{}','{}','{}',{},'{}','{}','{}',"
            "{},{},{},{},{},{},{},{})", actionId, DestinationActionType,
            _config.EnvironmentId, _config.RealmId, _config.PolicyVersion, payloadSha256,
            resultSha256, auctionId, selectedIdentity.Guid, auction->owner.GetCounter(),
            itemGuid.GetCounter(), auction->item_template, auction->itemCount, buyout, now);
        transaction->Append(
            "INSERT INTO la_outlands_action_audit "
            "(action_id,action_type,environment_id,realm_id,policy_version,payload_sha256,"
            "result_sha256,result,recorded_at_unix) VALUES ('{}','{}','{}',{},'{}','{}',"
            "'{}','applied',{})", actionId, DestinationActionType, _config.EnvironmentId,
            _config.RealmId, _config.PolicyVersion, payloadSha256, resultSha256, now);
        if (executeObserved.SellerKind == Procurement::OwnerKind::OwnedSystem)
        {
            transaction->Append(
                "INSERT INTO la_outlands_item_provenance "
                "(item_guid,purchase_action_id,source_seller_guid,receiving_ai_guid,"
                "reason,automation_resale_blocked,applied_at_unix) VALUES "
                "({},'{}',{},{},'owned_system_seed_purchase',1,{})",
                itemGuid.GetCounter(), actionId, auction->owner.GetCounter(), selectedIdentity.Guid, now);
        }
        CharacterDatabase.DirectCommitTransaction(transaction);

        DestinationReceiptRead verified = ReadDestinationReceipt(
            _config, actionId, payloadSha256, auction, selectedIdentity);
        if (verified.State != LivingAzeroth::Economy::DestinationReceipt::State::Applied ||
            verified.ResultSha256 != resultSha256)
        {
            buyer->SetMoney(walletBefore);
            auction->bidder = bidderBefore;
            auction->bid = bidBefore;
            _runtimeBlocked = true;
            LOG_FATAL("living_azeroth.economy",
                "Guild procurement unknown result actionId={} faction={} auction={} "
                "receiptState={} authoritativeReloadRequired=true.", actionId,
                FactionName(identity.Faction), auctionId,
                static_cast<std::uint32_t>(verified.State));
            World::StopNow(ERROR_EXIT_CODE);
            return;
        }

        sAuctionMgr->RemoveAItem(itemGuid);
        sScriptMgr->OnAuctionSuccessful(house, auction);
        house->RemoveAuction(auction);
        _claimedAuctionIds.insert(auctionId);
        ++_mutationsThisBoot;
        LOG_INFO("living_azeroth.economy",
            "Guild procurement applied actionId={} faction={} buyerGuid={} auction={} "
            "buyout={} bootAttempt={} payloadSha256={} resultSha256={} "
            "receiptReadback=true mutation=true.", actionId, FactionName(identity.Faction),
            selectedIdentity.Guid, auctionId, buyout, _mutationsThisBoot, payloadSha256,
            resultSha256);
    }

    void ReconcileSettlements()
    {
        std::uint64_t const now = GameTime::GetGameTime().count();
        QueryResult result = CharacterDatabase.Query(
            "SELECT p.action_key,p.auction_id,p.buyer_guid,p.seller_guid,p.item_guid,"
            "p.item_entry,p.item_count,p.buyout_copper,p.deposit_copper,p.cut_copper,"
            "p.wallet_before_copper,p.dispatched_at_unix,"
            "(SELECT COUNT(*) FROM auctionhouse a WHERE a.id=p.auction_id),"
            "(SELECT COUNT(*) FROM mail m JOIN mail_items mi ON mi.mail_id=m.id "
            " WHERE m.receiver=p.buyer_guid AND mi.item_guid=p.item_guid "
            " AND m.messageType=2 AND m.has_items<>0 "
            " AND m.subject=CONCAT(p.item_entry,':0:1:',p.auction_id,':',p.item_count)),"
            "(SELECT COUNT(*) FROM mail m WHERE m.receiver=p.seller_guid "
            " AND m.messageType=2 AND m.has_items=0 "
            " AND m.subject=CONCAT(p.item_entry,':0:2:',p.auction_id,':',p.item_count)),"
            "COALESCE((SELECT SUM(m.money) FROM mail m WHERE m.receiver=p.seller_guid "
            " AND m.messageType=2 AND m.has_items=0 "
            " AND m.subject=CONCAT(p.item_entry,':0:2:',p.auction_id,':',p.item_count)),0),"
            "(SELECT COUNT(*) FROM la_outlands_processed_action r "
            " WHERE r.action_id=p.action_key AND r.action_type='auction_buyout' "
            " AND r.auction_id=p.auction_id AND r.buyer_guid=p.buyer_guid "
            " AND r.seller_guid=p.seller_guid AND r.item_guid=p.item_guid "
            " AND r.item_count=p.item_count AND r.buyout_copper=p.buyout_copper) "
            "FROM la_guild_procurement_purchase p WHERE p.status='dispatched' "
            "AND p.dispatched_at_unix+5<={} ORDER BY p.dispatched_at_unix,p.auction_id LIMIT 20",
            now);
        if (!result)
            return;

        do
        {
            Field* fields = result->Fetch();
            std::string const actionKey = fields[0].Get<std::string>();
            std::uint32_t const auctionId = fields[1].Get<std::uint32_t>();
            std::uint32_t const buyerGuid = fields[2].Get<std::uint32_t>();
            std::uint64_t const totalPrice = fields[7].Get<std::uint64_t>();
            std::uint64_t const walletBefore = fields[10].Get<std::uint64_t>();
            std::uint64_t const recordedAt = fields[11].Get<std::uint64_t>();
            std::uint64_t const age = now >= recordedAt ? now - recordedAt : 0;
            SettlementObservation observed;
            observed.Action = SettlementActionKind::AuctionBuyout;
            observed.AgeSeconds = age;
            observed.TotalPrice = totalPrice;
            observed.Deposit = fields[8].Get<std::uint64_t>();
            observed.Cut = fields[9].Get<std::uint64_t>();
            observed.PurchaseAuctionReferences = fields[12].Get<std::uint32_t>();
            observed.WonMailCount = fields[13].Get<std::uint32_t>();
            observed.SellerMailCount = fields[14].Get<std::uint32_t>();
            observed.SellerMailMoney = fields[15].Get<std::uint64_t>();
            auto settlement = PlanSettlement(observed);
            bool const atomicAction = actionKey.starts_with("outlands:");
            bool const receiptApplied = fields[16].Get<std::uint32_t>() == 1;
            Player* buyer = atomicAction ? nullptr : FindLoadedBuyer(buyerGuid);
            bool const walletExpected = walletBefore >= totalPrice && buyer &&
                buyer->GetMoney() == walletBefore - totalPrice;
            char const* reason = settlement.Reason;
            if (atomicAction)
            {
                if (!receiptApplied)
                {
                    _runtimeBlocked = true;
                    LOG_FATAL("living_azeroth.economy",
                        "Guild procurement reconciliation quarantined actionKey={} auction={} "
                        "reason=destination_receipt_missing.", actionKey, auctionId);
                    continue;
                }
                settlement.Outcome = SettlementOutcome::Verified;
                reason = "destination_receipt_applied";
            }
            else if (settlement.Outcome == SettlementOutcome::Verified && !walletExpected)
            {
                settlement.Outcome = age >= 30 ? SettlementOutcome::Rejected :
                    SettlementOutcome::Pending;
                reason = buyer ? "buyer_wallet_mismatch" : "buyer_wallet_unavailable";
            }
            if (settlement.Outcome == SettlementOutcome::Pending)
                continue;

            char const* status = settlement.Outcome == SettlementOutcome::Verified ?
                "verified" : "rejected";
            std::uint64_t const buyerDebit = settlement.Outcome ==
                SettlementOutcome::Verified ? totalPrice : 0;
            std::uint64_t const sellerMail = observed.SellerMailMoney;
            std::string const evidence = EvidenceHash(
                actionKey + "|" + status + "|" + reason + "|" +
                std::to_string(auctionId) + "|" + std::to_string(buyerDebit) + "|" +
                std::to_string(sellerMail) + "|" + std::to_string(now));
            CharacterDatabaseTransaction transaction = CharacterDatabase.BeginTransaction();
            transaction->Append(SettlementUpdateSql, status, buyerDebit, sellerMail,
                reason, now, evidence, actionKey);
            CharacterDatabase.DirectCommitTransaction(transaction);
            LOG_INFO("living_azeroth.economy",
                "Guild procurement settlement status={} reason={} actionKey={} auction={} "
                "buyerDebit={} sellerMail={} walletExpected={}.", status, reason,
                actionKey, auctionId, buyerDebit, sellerMail, walletExpected);
        } while (result->NextRow());
    }

    RuntimeConfig _config;
    BuyerIdentity _allianceBuyer;
    BuyerIdentity _hordeBuyer;
    std::unordered_set<std::uint32_t> _ownedAiAccounts;
    std::unordered_set<std::uint32_t> _claimedAuctionIds;
    std::uint32_t _elapsedMs = 0;
    std::uint32_t _mutationsThisBoot = 0;
    bool _runtimeBlocked = true;
    bool _cycleConfigValid = false;
    bool _schemaReady = false;
    bool _catalogHashReady = false;
    bool _priceIndexReady = false;
    bool _authorityConfigValid = false;
};
}

void AddLivingAzerothGuildProcurementExecutorScripts()
{
    new LivingAzerothGuildProcurementExecutor();
}
