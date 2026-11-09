#include "LivingAzerothAutomatedListingBridge.h"

#include "AuctionHouseScript.h"
#include "Config.h"
#include "CryptoHash.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Item.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Util.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <string>
#include <unordered_map>

namespace
{
namespace Listing = LivingAzeroth::Economy::AutomatedListing;
constexpr char ListingConfirmation[] = "I_ACCEPT_ATOMIC_AI_LISTINGS_V1";
constexpr std::string_view ListingActionType = "auction_list";

bool gReady = false;
std::unordered_map<std::uint64_t, Listing::Pending> gPending;

std::uint64_t Key(std::uint32_t sellerGuid, std::uint32_t itemGuid)
{
    return (static_cast<std::uint64_t>(sellerGuid) << 32) | itemGuid;
}

std::string EvidenceHash(std::string const& payload)
{
    return ByteArrayToHexStr(Acore::Crypto::SHA256::GetDigestOf(payload));
}

bool Same(Listing::Pending const& left, Listing::Pending const& right)
{
    return left.ActionId == right.ActionId &&
        left.EnvironmentId == right.EnvironmentId && left.RealmId == right.RealmId &&
        left.PolicyVersion == right.PolicyVersion &&
        left.PayloadSha256 == right.PayloadSha256 &&
        left.SellerGuid == right.SellerGuid && left.SellerAccount == right.SellerAccount &&
        left.SourceItemGuid == right.SourceItemGuid && left.ItemEntry == right.ItemEntry &&
        left.ItemCount == right.ItemCount && left.HouseId == right.HouseId &&
        left.StartBid == right.StartBid && left.Buyout == right.Buyout &&
        left.Deposit == right.Deposit && left.DurationSeconds == right.DurationSeconds;
}

class LivingAzerothAutomatedListingConfig final : public WorldScript
{
public:
    LivingAzerothAutomatedListingConfig()
        : WorldScript("LivingAzerothAutomatedListingConfig",
            { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED,
              WORLDHOOK_ON_STARTUP }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        if (reload)
            Load();
    }

    void OnBeforeWorldInitialized() override { Load(); }
    void OnStartup() override { gPending.clear(); }

private:
    static void Load()
    {
        gReady = sConfigMgr->GetOption<bool>("LivingAzeroth.Economy.Enable", false) &&
            sConfigMgr->GetOption<bool>("LivingAzeroth.Economy.ProfileCommerce.Enable", false) &&
            !sConfigMgr->GetOption<bool>("LivingAzeroth.Economy.ProfileCommerce.ObserveOnly", true) &&
            sConfigMgr->GetOption<bool>("LivingAzeroth.Economy.ProfileCommerce.Seller.Enable", false) &&
            sConfigMgr->GetOption<bool>(
                "LivingAzeroth.Economy.ProfileCommerce.Seller.AtomicReceipt.Enable", false) &&
            sConfigMgr->GetOption<std::string>(
                "LivingAzeroth.Economy.ProfileCommerce.Seller.AtomicReceipt.Confirmation", "") ==
                ListingConfirmation;
        if (!gReady)
            gPending.clear();
        LOG_INFO("living_azeroth.economy",
            "Automated listing destination bridge ready={} pending={}.",
            gReady, gPending.size());
    }
};

class LivingAzerothAutomatedListingAuctionScript final : public AuctionHouseScript
{
public:
    LivingAzerothAutomatedListingAuctionScript()
        : AuctionHouseScript("LivingAzerothAutomatedListingAuctionScript",
            { AUCTIONHOUSEHOOK_CAN_AUTOMATED_LISTING_MUTATE,
              AUCTIONHOUSEHOOK_ON_AUTOMATED_LISTING_PRE_COMMIT }) { }

    bool CanAutomatedListingMutate(Player* player, Item* item, std::uint32_t count,
        std::uint32_t bid, std::uint32_t buyout, std::uint32_t durationSeconds,
        std::uint32_t deposit, bool fullStack) override
    {
        if (!gReady || !player->GetSession()->IsBot())
            return true;
        auto found = gPending.find(Key(player->GetGUID().GetCounter(),
            item->GetGUID().GetCounter()));
        if (found == gPending.end())
        {
            LOG_ERROR("living_azeroth.economy",
                "Automated listing rejected sellerGuid={} itemGuid={} reason=no_pending_action.",
                player->GetGUID().GetCounter(), item->GetGUID().GetCounter());
            return false;
        }
        Listing::Pending const& pending = found->second;
        bool const matches = fullStack && pending.SellerGuid == player->GetGUID().GetCounter() &&
            pending.SellerAccount == player->GetSession()->GetAccountId() &&
            pending.SourceItemGuid == item->GetGUID().GetCounter() &&
            pending.ItemEntry == item->GetEntry() && pending.ItemCount == count &&
            pending.StartBid == bid && pending.Buyout == buyout &&
            pending.Deposit == deposit && pending.DurationSeconds == durationSeconds;
        if (!matches)
            LOG_ERROR("living_azeroth.economy",
                "Automated listing rejected actionId={} sellerGuid={} itemGuid={} reason=payload_mismatch.",
                pending.ActionId, player->GetGUID().GetCounter(), item->GetGUID().GetCounter());
        return matches;
    }

    void OnAutomatedListingPreCommit(Player* player, Item* item, AuctionEntry* entry,
        CharacterDatabaseTransaction transaction) override
    {
        if (!gReady || !player->GetSession()->IsBot())
            return;
        auto found = gPending.find(Key(player->GetGUID().GetCounter(),
            item->GetGUID().GetCounter()));
        ASSERT(found != gPending.end());
        Listing::Pending const pending = found->second;
        ASSERT(entry->owner == player->GetGUID());
        ASSERT(static_cast<std::uint32_t>(entry->houseId) == pending.HouseId);
        std::uint64_t const now = GameTime::GetGameTime().count();
        std::string const resultSha256 = EvidenceHash(pending.ActionId + "|" +
            pending.PayloadSha256 + "|" + std::to_string(entry->Id) + "|applied|" +
            std::to_string(now));
        transaction->Append(
            "INSERT INTO la_outlands_auction_listing_receipt "
            "(action_id,action_type,environment_id,realm_id,policy_version,payload_sha256,"
            "result_sha256,auction_id,seller_guid,source_item_guid,item_entry,item_count,"
            "house_id,start_bid_copper,buyout_copper,deposit_copper,duration_seconds,"
            "applied_at_unix) VALUES ('{}','{}','{}',{},'{}','{}','{}',{},{},{},{},{},"
            "{},{},{},{},{},{})", pending.ActionId, ListingActionType,
            pending.EnvironmentId, pending.RealmId, pending.PolicyVersion,
            pending.PayloadSha256, resultSha256, entry->Id, pending.SellerGuid,
            pending.SourceItemGuid, pending.ItemEntry, pending.ItemCount, pending.HouseId,
            pending.StartBid, pending.Buyout, pending.Deposit, pending.DurationSeconds, now);
        transaction->Append(
            "INSERT INTO la_outlands_action_audit "
            "(action_id,action_type,environment_id,realm_id,policy_version,payload_sha256,"
            "result_sha256,result,recorded_at_unix) VALUES ('{}','{}','{}',{},'{}','{}',"
            "'{}','applied',{})", pending.ActionId, ListingActionType,
            pending.EnvironmentId, pending.RealmId, pending.PolicyVersion,
            pending.PayloadSha256, resultSha256, now);
        gPending.erase(found);
    }
};
}

namespace LivingAzeroth::Economy::AutomatedListing
{
bool Ready() { return gReady; }

bool Register(Pending const& pending)
{
    if (!gReady || pending.ActionId.empty() || pending.PayloadSha256.size() != 64 ||
        pending.SellerGuid == 0 || pending.SellerAccount == 0 ||
        pending.SourceItemGuid == 0 || pending.ItemEntry == 0 || pending.ItemCount == 0 ||
        pending.HouseId == 0 || pending.StartBid == 0 || pending.Buyout < pending.StartBid ||
        pending.DurationSeconds == 0)
        return false;
    auto [found, inserted] = gPending.emplace(
        Key(pending.SellerGuid, pending.SourceItemGuid), pending);
    return inserted || Same(found->second, pending);
}

void Forget(std::uint32_t sellerGuid, std::uint32_t sourceItemGuid)
{
    gPending.erase(Key(sellerGuid, sourceItemGuid));
}

ReceiptState ReadReceipt(Pending const& pending, std::string* resultSha256)
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT environment_id,realm_id,policy_version,payload_sha256,result_sha256,"
        "seller_guid,source_item_guid,item_entry,item_count,house_id,start_bid_copper,"
        "buyout_copper,deposit_copper,duration_seconds FROM "
        "la_outlands_auction_listing_receipt WHERE action_id='{}'", pending.ActionId);
    if (!result)
        return ReceiptState::Missing;
    Field* fields = result->Fetch();
    bool const matches = fields[0].Get<std::string>() == pending.EnvironmentId &&
        fields[1].Get<std::uint32_t>() == pending.RealmId &&
        fields[2].Get<std::string>() == pending.PolicyVersion &&
        fields[3].Get<std::string>() == pending.PayloadSha256 &&
        fields[5].Get<std::uint32_t>() == pending.SellerGuid &&
        fields[6].Get<std::uint32_t>() == pending.SourceItemGuid &&
        fields[7].Get<std::uint32_t>() == pending.ItemEntry &&
        fields[8].Get<std::uint32_t>() == pending.ItemCount &&
        fields[9].Get<std::uint32_t>() == pending.HouseId &&
        fields[10].Get<std::uint32_t>() == pending.StartBid &&
        fields[11].Get<std::uint32_t>() == pending.Buyout &&
        fields[12].Get<std::uint32_t>() == pending.Deposit &&
        fields[13].Get<std::uint32_t>() == pending.DurationSeconds;
    if (matches && resultSha256)
        *resultSha256 = fields[4].Get<std::string>();
    return matches ? ReceiptState::Applied : ReceiptState::Conflict;
}

bool IsAutomationResaleBlocked(std::uint32_t itemGuid)
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT automation_resale_blocked FROM la_outlands_item_provenance "
        "WHERE item_guid={}", itemGuid);
    return result && result->Fetch()[0].Get<std::uint32_t>() == 1;
}
}

void AddLivingAzerothAutomatedListingBridgeScripts()
{
    new LivingAzerothAutomatedListingConfig();
    new LivingAzerothAutomatedListingAuctionScript();
}
