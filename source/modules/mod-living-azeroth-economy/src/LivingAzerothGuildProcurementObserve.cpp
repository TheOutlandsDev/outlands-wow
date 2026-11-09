#include "LivingAzerothEconomyPlan.h"
#include "LivingAzerothFullPriceIndex.h"
#include "LivingAzerothGuildProcurementPlan.h"

#include "AuctionHouseMgr.h"
#include "CharacterCache.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "World.h"
#include "WorldScript.h"

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

constexpr char ConfirmationToken[] = "I_ACCEPT_OBSERVE_ONLY_GUILD_PROCUREMENT_V1";
constexpr std::size_t ReasonCount =
    static_cast<std::size_t>(Procurement::Reason::FactionCycleCap) + 1;

char const* ReasonName(Procurement::Reason reason)
{
    switch (reason)
    {
        case Procurement::Reason::ObserveOnlyApproved: return "observe_only_approved";
        case Procurement::Reason::FeatureDisabled: return "feature_disabled";
        case Procurement::Reason::MutationModeRejected: return "mutation_mode_rejected";
        case Procurement::Reason::ConfirmationMissing: return "confirmation_missing";
        case Procurement::Reason::UnsupportedFaction: return "unsupported_faction";
        case Procurement::Reason::InvalidAuction: return "invalid_auction";
        case Procurement::Reason::SellerNotHuman: return "seller_not_human";
        case Procurement::Reason::SellerOwnedAi: return "seller_owned_ai";
        case Procurement::Reason::SellerOwnedSystem: return "seller_owned_system";
        case Procurement::Reason::OwnedSystemSellerDisabled: return "owned_system_seller_disabled";
        case Procurement::Reason::OwnedSystemSellerNotAllowlisted: return "owned_system_seller_not_allowlisted";
        case Procurement::Reason::OwnedSystemDailyPurchaseCap: return "owned_system_daily_purchase_cap";
        case Procurement::Reason::SellerUnknown: return "seller_unknown";
        case Procurement::Reason::SelfTrade: return "self_trade";
        case Procurement::Reason::CatalogRejected: return "catalog_rejected";
        case Procurement::Reason::UnsafeItem: return "unsafe_item";
        case Procurement::Reason::BidPresent: return "bid_present";
        case Procurement::Reason::DuplicateAuction: return "duplicate_auction";
        case Procurement::Reason::MissingReferencePrice: return "missing_reference_price";
        case Procurement::Reason::PurchasePriceCeiling: return "purchase_price_ceiling";
        case Procurement::Reason::FactionDailyPurchaseCap: return "faction_daily_purchase_cap";
        case Procurement::Reason::FactionDailySpendCap: return "faction_daily_spend_cap";
        case Procurement::Reason::SellerDailyPurchaseCap: return "seller_daily_purchase_cap";
        case Procurement::Reason::FactionCycleCap: return "faction_cycle_cap";
    }
    return "unknown";
}

char const* FactionName(Procurement::Faction faction)
{
    return faction == Procurement::Faction::Alliance ? "Alliance" :
        faction == Procurement::Faction::Horde ? "Horde" : "Other";
}

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
            unsigned long const parsed = std::stoul(token, &consumed, 10);
            if (consumed == token.size() && parsed > 0 &&
                parsed <= std::numeric_limits<std::uint32_t>::max())
                result.insert(static_cast<std::uint32_t>(parsed));
        }
        catch (...)
        {
            LOG_WARN("living_azeroth.economy",
                "Guild procurement ignored invalid system seller GUID token '{}'.", token);
        }
    }
    return result;
}

struct RuntimeConfig
{
    bool MasterEnabled = false;
    Procurement::Policy Policy;
    std::uint32_t CycleMs = 60 * IN_MILLISECONDS;
    std::unordered_set<std::uint32_t> SystemSellerGuids;
};

struct CandidateRuntime
{
    Procurement::Candidate Planned;
    std::uint32_t ItemEntry = 0;
    std::string ItemName;
    std::string SellerName;
};

class LivingAzerothGuildProcurementObserve final : public WorldScript
{
public:
    LivingAzerothGuildProcurementObserve()
        : WorldScript("LivingAzerothGuildProcurementObserve",
            { WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED, WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE })
    {
    }

    void OnBeforeWorldInitialized() override
    {
        LoadConfig();
    }

    void OnStartup() override
    {
        _elapsedMs = 0;
        _ownedAiAccounts.clear();
        QueryResult result = PlayerbotsDatabase.Query(
            "SELECT account_id FROM playerbots_account_type WHERE account_type IN (1,2) ORDER BY account_id");
        if (result)
        {
            do
            {
                _ownedAiAccounts.insert(result->Fetch()[0].Get<std::uint32_t>());
            } while (result->NextRow());
        }
        _runtimeBlocked = _runtimeBlocked || _ownedAiAccounts.empty() ||
            !LivingAzeroth::Economy::FullPriceIndex::Instance().EnsureLoaded();
        ResetSimulationIfNeeded();
        LOG_INFO("living_azeroth.economy",
            "Guild procurement observer startup: enabled={} observeOnly={} runtimeBlocked={} "
            "ownedAiAccounts={} mutationPathCompiled=false.",
            _config.Policy.Enabled, _config.Policy.ObserveOnly, _runtimeBlocked,
            _ownedAiAccounts.size());
    }

    void OnUpdate(std::uint32_t diff) override
    {
        if (_runtimeBlocked)
            return;
        _elapsedMs += diff;
        if (_elapsedMs < _config.CycleMs)
            return;
        _elapsedMs = 0;
        ResetSimulationIfNeeded();
        ObserveHouse(AuctionHouseId::Alliance, Procurement::Faction::Alliance, 0);
        ObserveHouse(AuctionHouseId::Horde, Procurement::Faction::Horde, 1);
    }

private:
    void LoadConfig()
    {
        _runtimeBlocked = true;
        _config.MasterEnabled = sConfigMgr->GetOption<bool>("LivingAzeroth.Economy.Enable", false);
        _config.Policy.Enabled = _config.MasterEnabled && sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.GuildProcurement.Enable", false);
        _config.Policy.ObserveOnly = sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.GuildProcurement.ObserveOnly", true);
        std::string const confirmation = sConfigMgr->GetOption<std::string>(
            "LivingAzeroth.Economy.GuildProcurement.Confirmation", "");
        _config.Policy.ConfirmationValid = confirmation == ConfirmationToken;
        _config.Policy.RequireCatalogApproval = sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.GuildProcurement.RequireCatalogApproval", true);
        _config.Policy.AllowOwnedSystemSellerAuctions = sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.GuildProcurement.AllowOwnedSystemSellerAuctions", false);
        std::uint32_t const ceilingPercent = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.GuildProcurement.CeilingPercent", 110);
        std::uint32_t const factionDailyCount = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.GuildProcurement.MaxPurchasesPerFactionPerDay", 5);
        std::uint64_t const factionDailySpend = sConfigMgr->GetOption<std::uint64_t>(
            "LivingAzeroth.Economy.GuildProcurement.MaxSpendCopperPerFactionPerDay", 50000);
        std::uint32_t const sellerDailyCount = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.GuildProcurement.MaxPurchasesPerSellerPerDay", 2);
        std::uint32_t const systemDailyCount = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.GuildProcurement.MaxOwnedSystemPurchasesPerFactionPerDay", 20);
        std::uint32_t const factionCycleCount = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.GuildProcurement.MaxProposalsPerFactionPerCycle", 1);
        std::uint32_t const cycleSeconds = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.GuildProcurement.CycleSeconds", 60);
        bool const numericConfigValid = ceilingPercent >= 50 && ceilingPercent <= 125 &&
            factionDailyCount <= 120 &&
            factionDailySpend >= 1 && factionDailySpend <= 600000 &&
            sellerDailyCount <= 100 &&
            systemDailyCount >= 1 && systemDailyCount <= 30 &&
            factionCycleCount >= 1 && factionCycleCount <= 3 &&
            cycleSeconds >= 30 && cycleSeconds <= 3600;
        _config.Policy.CeilingPercent = ceilingPercent;
        _config.Policy.MaxPurchasesPerFactionPerDay = factionDailyCount;
        _config.Policy.MaxSpendPerFactionPerDay = factionDailySpend;
        _config.Policy.MaxPurchasesPerSellerPerDay = sellerDailyCount;
        _config.Policy.MaxOwnedSystemPurchasesPerFactionPerDay = systemDailyCount;
        _config.Policy.MaxProposalsPerFactionPerCycle = factionCycleCount;
        _config.CycleMs = std::clamp(cycleSeconds, std::uint32_t(30),
            std::uint32_t(3600)) * IN_MILLISECONDS;
        _config.SystemSellerGuids = ParseGuidSet(sConfigMgr->GetOption<std::string>(
            "LivingAzeroth.Economy.GuildProcurement.OwnedSystemSellerGuids", "900001"));

        _runtimeBlocked = !_config.Policy.Enabled || !_config.Policy.ObserveOnly ||
            !_config.Policy.ConfirmationValid || !numericConfigValid ||
            _config.Policy.MaxSpendPerFactionPerDay == 0;
        LOG_INFO("living_azeroth.economy",
            "Guild procurement observer config: master={} enabled={} observeOnly={} confirmation={} "
            "ceiling={} dailyCount={} dailySpend={} sellerCount={} cycleCount={} runtimeBlocked={}.",
            _config.MasterEnabled, _config.Policy.Enabled, _config.Policy.ObserveOnly,
            _config.Policy.ConfirmationValid, _config.Policy.CeilingPercent,
            _config.Policy.MaxPurchasesPerFactionPerDay,
            _config.Policy.MaxSpendPerFactionPerDay,
            _config.Policy.MaxPurchasesPerSellerPerDay,
            _config.Policy.MaxProposalsPerFactionPerCycle, _runtimeBlocked);
    }

    void ResetSimulationIfNeeded()
    {
        std::uint64_t const day = static_cast<std::uint64_t>(GameTime::GetGameTime().count()) / DAY;
        if (_simulationDay == day)
            return;
        _simulationDay = day;
        _factionPurchases = {};
        _factionSpend = {};
        _ownedSystemPurchases = {};
        _sellerPurchases.clear();
        _observedAuctionIds.clear();
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

    void ObserveHouse(AuctionHouseId houseId, Procurement::Faction faction, std::size_t factionIndex)
    {
        AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(houseId);
        std::vector<CandidateRuntime> candidates;
        std::array<std::uint32_t, ReasonCount> reasons{};
        std::uint32_t scanned = 0;
        for (auto const& [auctionId, auction] : house->GetAuctions())
        {
            (void)auctionId;
            if (!auction)
                continue;
            ++scanned;
            CandidateRuntime candidate;
            candidate.Planned.AuctionId = auction->Id;
            candidate.ItemEntry = auction->item_template;
            ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(auction->item_template);
            candidate.ItemName = itemTemplate ? itemTemplate->Name1 : "unknown";
            Procurement::Observation& observed = candidate.Planned.Observed;
            observed.MarketFaction = faction;
            observed.SellerKind = ClassifyOwner(auction->owner, observed.SellerAccount,
                candidate.SellerName);
            observed.SellerSystemAllowlisted = observed.SellerKind ==
                Procurement::OwnerKind::OwnedSystem &&
                _config.SystemSellerGuids.contains(auction->owner.GetCounter());
            observed.CatalogApproved = LivingAzeroth::Economy::FullPriceIndex::Instance().Approved(
                auction->item_template);
            observed.SafeItem = SafeItem(auction, sAuctionMgr->GetAItem(auction->item_guid),
                itemTemplate);
            observed.HasBid = static_cast<bool>(auction->bidder) || auction->bid != 0;
            observed.DuplicateAuction = _observedAuctionIds.contains(auction->Id);
            observed.SellerGuid = auction->owner.GetCounter();
            observed.ItemCount = auction->itemCount;
            observed.FactionPurchasesToday = _factionPurchases[factionIndex];
            observed.SellerPurchasesToday = _sellerPurchases[
                (static_cast<std::uint64_t>(factionIndex) << 32) | observed.SellerGuid];
            observed.OwnedSystemPurchasesToday = _ownedSystemPurchases[factionIndex];
            observed.FactionProposalsThisCycle = 0;
            observed.ReferenceUnitPrice = LivingAzeroth::Economy::FullPriceIndex::Instance().ProcurementUnitPrice(
                auction->item_template);
            observed.Buyout = auction->buyout;
            observed.FactionSpendToday = _factionSpend[factionIndex];
            Procurement::Decision const decision = Procurement::Plan(_config.Policy, observed);
            ++reasons[static_cast<std::size_t>(decision.Why)];
            candidates.push_back(std::move(candidate));
        }

        std::vector<Procurement::Candidate> pureCandidates;
        pureCandidates.reserve(candidates.size());
        for (CandidateRuntime const& candidate : candidates)
            pureCandidates.push_back(candidate.Planned);
        Procurement::Selection const selected = Procurement::Select(_config.Policy, pureCandidates);
        std::uint32_t proposed = 0;
        if (selected.Found)
        {
            auto found = std::find_if(candidates.begin(), candidates.end(), [&](CandidateRuntime const& row)
            {
                return row.Planned.AuctionId == selected.AuctionId;
            });
            if (found != candidates.end())
            {
                ++proposed;
                Procurement::Observation const& observed = found->Planned.Observed;
                _observedAuctionIds.insert(selected.AuctionId);
                ++_factionPurchases[factionIndex];
                _factionSpend[factionIndex] += observed.Buyout;
                ++_sellerPurchases[(static_cast<std::uint64_t>(factionIndex) << 32) |
                    observed.SellerGuid];
                if (observed.SellerKind == Procurement::OwnerKind::OwnedSystem)
                    ++_ownedSystemPurchases[factionIndex];
                LOG_INFO("living_azeroth.economy",
                    "Guild procurement proposal mode=observe-only mutation=false faction={} auction={} "
                    "seller={} sellerGuid={} item={} itemEntry={} count={} buyout={} "
                    "referenceLot={} ceiling={} reason={}.",
                    FactionName(faction), selected.AuctionId, found->SellerName,
                    observed.SellerGuid, found->ItemName, found->ItemEntry, observed.ItemCount,
                    observed.Buyout, selected.Planned.ReferenceLotPrice,
                    selected.Planned.CeilingPrice, ReasonName(selected.Planned.Why));
            }
        }

        LOG_INFO("living_azeroth.economy",
            "Guild procurement cycle mode=observe-only mutation=false faction={} scanned={} proposed={} "
            "simulatedDailyCount={} simulatedDailySpend={} humanEligible={} systemRejected={} "
            "aiRejected={} unknownRejected={} catalogRejected={} unsafeRejected={} bidRejected={} "
            "duplicateRejected={} priceRejected={} countCapRejected={} spendCapRejected={} "
            "sellerCapRejected={}.",
            FactionName(faction), scanned, proposed, _factionPurchases[factionIndex],
            _factionSpend[factionIndex],
            reasons[static_cast<std::size_t>(Procurement::Reason::ObserveOnlyApproved)],
            reasons[static_cast<std::size_t>(Procurement::Reason::SellerOwnedSystem)],
            reasons[static_cast<std::size_t>(Procurement::Reason::SellerOwnedAi)],
            reasons[static_cast<std::size_t>(Procurement::Reason::SellerUnknown)],
            reasons[static_cast<std::size_t>(Procurement::Reason::CatalogRejected)],
            reasons[static_cast<std::size_t>(Procurement::Reason::UnsafeItem)],
            reasons[static_cast<std::size_t>(Procurement::Reason::BidPresent)],
            reasons[static_cast<std::size_t>(Procurement::Reason::DuplicateAuction)],
            reasons[static_cast<std::size_t>(Procurement::Reason::PurchasePriceCeiling)],
            reasons[static_cast<std::size_t>(Procurement::Reason::FactionDailyPurchaseCap)],
            reasons[static_cast<std::size_t>(Procurement::Reason::FactionDailySpendCap)],
            reasons[static_cast<std::size_t>(Procurement::Reason::SellerDailyPurchaseCap)]);
    }

    RuntimeConfig _config;
    bool _runtimeBlocked = true;
    std::uint32_t _elapsedMs = 0;
    std::uint64_t _simulationDay = std::numeric_limits<std::uint64_t>::max();
    std::unordered_set<std::uint32_t> _ownedAiAccounts;
    std::unordered_set<std::uint32_t> _observedAuctionIds;
    std::array<std::uint32_t, 2> _factionPurchases{};
    std::array<std::uint64_t, 2> _factionSpend{};
    std::array<std::uint32_t, 2> _ownedSystemPurchases{};
    std::unordered_map<std::uint64_t, std::uint32_t> _sellerPurchases;
};
}

void AddLivingAzerothGuildProcurementObserveScripts()
{
    new LivingAzerothGuildProcurementObserve();
}
