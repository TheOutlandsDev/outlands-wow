/*
 * Living Azeroth AI4 deterministic, seller-only auction reconciler.
 */

#include "LivingAzerothEconomyPlan.h"
#include "LivingAzerothEconomyProvisionPlan.h"
#include "LivingAzerothExpiredMailPlan.h"
#include "LivingAzerothFullPriceIndex.h"
#include "LivingAzerothMarketDensityCatalog.h"
#include "LivingAzerothProfessionItemCatalog.h"
#include "LivingAzerothRotationExpiredMailPlan.h"
#include "LivingAzerothRotationMarketPlan.h"

#include "AccountMgr.h"
#include "AuctionHouseMgr.h"
#include "CharacterCache.h"
#include "Config.h"
#include "CryptoRandom.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "World.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <array>
#include <charconv>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using LivingAzeroth::Economy::BuildExpiredMailPlan;
using LivingAzeroth::Economy::BuildProvisionPlan;
using LivingAzeroth::Economy::BuildRotationExpiredMailPlan;
using LivingAzeroth::Economy::BuildRotationMarketPlan;
using LivingAzeroth::Economy::BuildRotationReconcilePlan;
using LivingAzeroth::Economy::ApplyLiveListingCeiling;
using LivingAzeroth::Economy::BuildReconcilePlan;
using LivingAzeroth::Economy::BusinessKey;
using LivingAzeroth::Economy::CatalogRow;
using LivingAzeroth::Economy::ExpiredMailObservation;
using LivingAzeroth::Economy::FullPriceCatalogSha256;
using LivingAzeroth::Economy::FullPriceIndex;
using LivingAzeroth::Economy::ObservedAuction;
using LivingAzeroth::Economy::ProvisionAccountName;
using LivingAzeroth::Economy::ProvisionCharacterName;
using LivingAzeroth::Economy::ProvisionClass;
using LivingAzeroth::Economy::ProvisionGender;
using LivingAzeroth::Economy::ProvisionObservation;
using LivingAzeroth::Economy::ProvisionPlan;
using LivingAzeroth::Economy::ProvisionRace;
using LivingAzeroth::Economy::RotationExpirationSeconds;
using LivingAzeroth::Economy::RotationMetric;
using LivingAzeroth::Economy::RotationPlan;
using LivingAzeroth::Economy::StaticCatalog;
using LivingAzeroth::Economy::StaticCatalogSha256;
using LivingAzeroth::Economy::StaticMarkets;

constexpr std::uint32_t StaticAuctionDurationHours = 48;
constexpr std::uint32_t StaticAuctionDurationSeconds = StaticAuctionDurationHours * 60 * 60;
constexpr std::chrono::seconds ProvisionDatabaseTimeout = std::chrono::seconds(30);

struct PendingAuction
{
    Item* item;
    AuctionEntry* auction;
    AuctionHouseObject* house;
};

struct PendingRetirement
{
    Item* item;
    AuctionEntry* auction;
    AuctionHouseObject* house;
};

class LivingAzerothEconomyController
{
public:
    static LivingAzerothEconomyController& Instance()
    {
        static LivingAzerothEconomyController instance;
        return instance;
    }

    void OnConfigLoad()
    {
        _reconcileRequested = true;
    }

    void RequestReconcileForStaticAuction(AuctionEntry const* auction)
    {
        if (!auction || _sellerGuid == 0 ||
            auction->owner.GetCounter() != _sellerGuid ||
            !sConfigMgr->GetOption<bool>(
                "LivingAzeroth.Economy.StaticSeller.RelistOnExpire.Enable", false))
        {
            return;
        }

        _reconcileRequested = true;
    }

    void ReconcileIfRequested()
    {
        if (!_reconcileRequested || _running)
            return;

        if (!sConfigMgr->GetOption<bool>("LivingAzeroth.Economy.Enable", false) ||
            !sConfigMgr->GetOption<bool>("LivingAzeroth.Economy.StaticSeller.Enable", false))
        {
            return;
        }

        _reconcileRequested = false;
        _running = true;
        struct RunningGuard
        {
            bool& running;
            ~RunningGuard() { running = false; }
        } guard { _running };

        if (sConfigMgr->GetOption<bool>("AuctionHouseBot.EnableSeller", false) ||
            sConfigMgr->GetOption<bool>("AuctionHouseBot.EnableBuyer", false))
        {
            Fail("stock_ahbot_seller_or_buyer_enabled");
            return;
        }

        if (sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION))
        {
            Fail("cross_faction_auction_mode_collapses_locked_markets");
            return;
        }


        std::uint32_t const durationHours = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.StaticSeller.DurationHours", 0);
        if (durationHours != StaticAuctionDurationHours)
        {
            Fail("duration_hours_must_equal_48");
            return;
        }

        std::string const configuredCatalogHash = sConfigMgr->GetOption<std::string>(
            "LivingAzeroth.Economy.StaticSeller.CatalogSha256", "");
        if (configuredCatalogHash != FullPriceCatalogSha256)
        {
            Fail("catalog_sha256_mismatch");
            return;
        }

        std::uint32_t account = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.StaticSeller.Account", 0);
        std::uint32_t guid = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.StaticSeller.GUID", 0);
        bool const provision =
            sConfigMgr->GetOption<bool>("LivingAzeroth.Economy.Provision.Enable", false);

        if (provision)
        {
            if (account != 0 || guid != 0)
            {
                FailProvision("configured_account_and_guid_must_be_zero");
                return;
            }

            std::string const accountName = sConfigMgr->GetOption<std::string>(
                "LivingAzeroth.Economy.Provision.AccountName", "");
            std::string const characterName = sConfigMgr->GetOption<std::string>(
                "LivingAzeroth.Economy.Provision.CharacterName", "");
            if (accountName != ProvisionAccountName || characterName != ProvisionCharacterName)
            {
                FailProvision("locked_identity_name_mismatch");
                return;
            }

            if (!ResolveOrProvisionSeller(account, guid))
                return;
        }
        else if (account == 0 || guid == 0)
        {
            Fail("seller_account_and_guid_required");
            return;
        }

        if (sWorldSessionMgr->FindSession(account))
        {
            _reconcileRequested = true;
            LOG_INFO(
                "module.living_azeroth.economy",
                "Living Azeroth AI4 static seller reconciliation result=DEFERRED "
                "reason=locked_seller_session_present account={} guid={}",
                account,
                guid);
            return;
        }

        FullPriceIndex& priceIndex = FullPriceIndex::Instance();
        if (!priceIndex.EnsureLoaded())
        {
            Fail(priceIndex.Error());
            return;
        }
        std::uint32_t const listingTier = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.StaticSeller.ListingTier", 0);
        std::uint32_t const maxLiveListings = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.StaticSeller.MaxLiveListings", 0);
        if (maxLiveListings < listingTier || maxLiveListings > 12000)
        {
            Fail("max_live_listings_out_of_bounds");
            return;
        }
        std::uint64_t const rotationEpoch =
            static_cast<std::uint64_t>(GameTime::GetGameTime().count()) /
            StaticAuctionDurationSeconds;
        RotationPlan rotation = BuildCurrentRotation(listingTier, rotationEpoch);
        RotationPlan previous = BuildCurrentRotation(
            listingTier, rotationEpoch == 0 ? 0 : rotationEpoch - 1);
        if (!rotation.ok || !previous.ok)
        {
            Fail(rotation.ok ? previous.error : rotation.error);
            return;
        }
        std::vector<BusinessKey> allowedMailKeys = rotation.desired;
        allowedMailKeys.insert(
            allowedMailKeys.end(), previous.desired.begin(), previous.desired.end());
        for (std::uint8_t market : StaticMarkets)
        {
            for (auto const* row : priceIndex.ApprovedRows())
            {
                allowedMailKeys.push_back({
                    market,
                    row->ItemEntry,
                    row->RecommendedStack,
                    0,
                    0
                });
            }
        }

        if (!ValidateSellerIdentity(account, guid))
            return;

        _sellerGuid = guid;
        std::size_t reclaimedMail = 0;
        if (!ReclaimExpiredMail(guid, allowedMailKeys, listingTier, reclaimedMail))
            return;

        std::string accountName = "LivingAzerothEconomy" + std::to_string(account);
        WorldSession session(
            account,
            std::move(accountName),
            0,
            nullptr,
            SEC_PLAYER,
            sWorld->getIntConfig(CONFIG_EXPANSION),
            0,
            LOCALE_enUS,
            0,
            false,
            false,
            0);
        Player seller(&session);
        seller.Initialize(guid);
        ObjectAccessor::AddObject(&seller);

        ReconcileWithSeller(seller, rotation, rotationEpoch, reclaimedMail, maxLiveListings);

        ObjectAccessor::RemoveObject(&seller);
    }

private:
    bool _reconcileRequested = true;
    bool _running = false;
    std::uint32_t _sellerGuid = 0;

    static void Fail(std::string_view reason)
    {
        LOG_ERROR(
            "module.living_azeroth.economy",
            "Living Azeroth AI4 static seller reconciliation result=FAIL reason={}",
            reason);
    }

    static void FailProvision(std::string_view reason)
    {
        LOG_ERROR(
            "module.living_azeroth.economy",
            "Living Azeroth AI4 seller provision result=FAIL reason={}",
            reason);
    }

    static bool ObserveProvisionState(ProvisionObservation& observed)
    {
        observed = {};

        std::string const accountName(ProvisionAccountName);
        observed.accountId = AccountMgr::GetId(accountName);
        observed.accountExists = observed.accountId != 0;
        if (observed.accountExists)
        {
            std::string storedAccountName;
            if (!AccountMgr::GetName(observed.accountId, storedAccountName) ||
                storedAccountName != ProvisionAccountName)
            {
                FailProvision("seller_account_name_collision");
                return false;
            }

            observed.accountSecurity = AccountMgr::GetSecurity(observed.accountId);
            observed.accountCharacterCount =
                AccountMgr::GetCharactersCount(observed.accountId);
        }

        CharacterDatabasePreparedStatement* statement =
            CharacterDatabase.GetPreparedStatement(CHAR_SEL_DATA_BY_NAME);
        statement->SetData(0, std::string(ProvisionCharacterName));
        PreparedQueryResult result = CharacterDatabase.Query(statement);
        if (!result)
            return true;

        Field* fields = result->Fetch();
        std::string const storedCharacterName = fields[2].Get<std::string>();
        if (storedCharacterName != ProvisionCharacterName)
        {
            FailProvision("seller_character_name_collision");
            return false;
        }

        observed.characterExists = true;
        observed.characterGuid = fields[0].Get<std::uint32_t>();
        observed.characterAccountId = fields[1].Get<std::uint32_t>();
        observed.characterGender = fields[3].Get<std::uint8_t>();
        observed.characterRace = fields[4].Get<std::uint8_t>();
        observed.characterClass = fields[5].Get<std::uint8_t>();
        return true;
    }

    static std::string CreateUnrecoverablePassword()
    {
        std::array<std::uint8_t, 16> bytes = Acore::Crypto::GetRandomBytes<16>();
        constexpr std::string_view alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

        std::string password;
        password.reserve(bytes.size());
        for (std::uint8_t byte : bytes)
            password.push_back(alphabet[byte % alphabet.size()]);

        return password;
    }

    static bool AwaitCharacterCommit(CharacterDatabaseTransaction transaction)
    {
        TransactionCallback callback =
            CharacterDatabase.AsyncCommitTransaction(std::move(transaction));
        if (callback.m_future.wait_for(ProvisionDatabaseTimeout) !=
            std::future_status::ready)
        {
            return false;
        }

        return callback.m_future.get();
    }

    static bool WaitForAccount(std::uint32_t& accountId)
    {
        auto const deadline = std::chrono::steady_clock::now() + ProvisionDatabaseTimeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            accountId = AccountMgr::GetId(std::string(ProvisionAccountName));
            if (accountId != 0)
                return true;

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        FailProvision("seller_account_persistence_timeout");
        return false;
    }

    static bool CreateProvisionedCharacter(std::uint32_t accountId)
    {
        WorldSession session(
            accountId,
            std::string(ProvisionAccountName),
            0,
            nullptr,
            SEC_PLAYER,
            sWorld->getIntConfig(CONFIG_EXPANSION),
            0,
            LOCALE_enUS,
            0,
            false,
            false,
            0);

        auto character = std::make_unique<Player>(&session);
        character->GetMotionMaster()->Initialize();
        CharacterCreateInfo createInfo(
            std::string(ProvisionCharacterName),
            ProvisionRace,
            ProvisionClass,
            ProvisionGender,
            0,
            0,
            0,
            0,
            0);

        if (!character->Create(
                sObjectMgr->GetGenerator<HighGuid::Player>().Generate(),
                &createInfo))
        {
            character->CleanupsBeforeDelete();
            FailProvision("seller_character_creation_failed");
            return false;
        }

        character->setCinematic(2);
        character->SetAtLoginFlag(AT_LOGIN_NONE);

        CharacterDatabaseTransaction transaction = CharacterDatabase.BeginTransaction();
        character->SaveToDB(transaction, true, false);
        if (!AwaitCharacterCommit(std::move(transaction)))
        {
            character->CleanupsBeforeDelete();
            FailProvision("seller_character_persistence_failed");
            return false;
        }

        sCharacterCache->AddCharacterCacheEntry(
            character->GetGUID(),
            accountId,
            character->GetName(),
            character->getGender(),
            character->getRace(),
            character->getClass(),
            character->GetLevel());
        character->CleanupsBeforeDelete();
        return true;
    }

    static bool ResolveOrProvisionSeller(std::uint32_t& account, std::uint32_t& guid)
    {
        ProvisionObservation observed;
        if (!ObserveProvisionState(observed))
            return false;

        ProvisionPlan plan = BuildProvisionPlan(observed);
        if (!plan.ok)
        {
            FailProvision(plan.error);
            return false;
        }

        bool accountCreated = false;
        bool characterCreated = false;
        if (plan.createAccount)
        {
            std::string password = CreateUnrecoverablePassword();
            AccountOpResult const result = sAccountMgr->CreateAccount(
                std::string(ProvisionAccountName),
                std::move(password));
            if (result != AOR_OK)
            {
                FailProvision("seller_account_creation_failed");
                return false;
            }

            accountCreated = true;
            if (!WaitForAccount(account))
                return false;

            if (!ObserveProvisionState(observed))
                return false;
            plan = BuildProvisionPlan(observed);
            if (!plan.ok || plan.createAccount)
            {
                FailProvision("seller_account_post_create_validation_failed");
                return false;
            }
        }
        else
        {
            account = plan.accountId;
        }

        if (plan.createCharacter)
        {
            if (!CreateProvisionedCharacter(account))
                return false;

            characterCreated = true;
            if (!ObserveProvisionState(observed))
                return false;
            plan = BuildProvisionPlan(observed);
            if (!plan.ok || plan.createAccount || plan.createCharacter)
            {
                FailProvision("seller_character_post_create_validation_failed");
                return false;
            }
        }

        account = plan.accountId;
        guid = plan.characterGuid;
        LOG_INFO(
            "module.living_azeroth.economy",
            "Living Azeroth AI4 seller provision result=PASS account_created={} "
            "character_created={} account={} guid={} password_persisted=0",
            accountCreated ? 1 : 0,
            characterCreated ? 1 : 0,
            account,
            guid);
        return true;
    }

    static bool ValidateSellerIdentity(std::uint32_t account, std::uint32_t guid)
    {
        CharacterDatabasePreparedStatement* statement =
            CharacterDatabase.GetPreparedStatement(CHAR_SEL_DATA_BY_GUID);
        statement->SetData(0, guid);
        PreparedQueryResult result = CharacterDatabase.Query(statement);
        if (!result)
        {
            Fail("seller_character_not_found");
            return false;
        }

        Field* fields = result->Fetch();
        if (fields[0].Get<std::uint32_t>() != guid ||
            fields[1].Get<std::uint32_t>() != account)
        {
            Fail("seller_character_account_mismatch");
            return false;
        }

        return true;
    }

    static RotationPlan BuildCurrentRotation(std::uint32_t tier, std::uint64_t epoch)
    {
        std::vector<RotationMetric> metrics;
        for (auto const* row : FullPriceIndex::Instance().ApprovedRows())
        {
            metrics.push_back({
                row->ItemEntry,
                row->RecommendedStack,
                row->ReferenceUnitCopper,
                row->ModeledMinUnitCopper,
                row->ModeledMaxUnitCopper,
                LivingAzeroth::Economy::ProfessionItemCatalog::Contains(row->ItemEntry),
                static_cast<std::uint8_t>(
                    LivingAzeroth::Economy::MarketDensityCatalog::IsLowTier(row->ItemEntry) ? 2 :
                    LivingAzeroth::Economy::MarketDensityCatalog::IsOtherCommodity(row->ItemEntry) ? 1 : 0)
            });
        }
        return BuildRotationMarketPlan(metrics, tier, epoch);
    }

    static bool ParseAuctionMailSubject(
        std::string const& subject,
        std::array<std::uint32_t, 5>& values)
    {
        std::size_t start = 0;
        for (std::size_t index = 0; index < values.size(); ++index)
        {
            std::size_t const end = subject.find(':', start);
            if ((index + 1 < values.size() && end == std::string::npos) ||
                (index + 1 == values.size() && end != std::string::npos))
            {
                return false;
            }

            std::size_t const length =
                end == std::string::npos ? subject.size() - start : end - start;
            if (length == 0)
                return false;

            std::string_view const token(subject.data() + start, length);
            auto const [pointer, error] = std::from_chars(
                token.data(), token.data() + token.size(), values[index]);
            if (error != std::errc() || pointer != token.data() + token.size())
                return false;

            start = end == std::string::npos ? subject.size() : end + 1;
        }

        return start == subject.size();
    }

    static bool ReclaimExpiredMail(std::uint32_t sellerGuid, std::span<BusinessKey const> allowed,
        std::uint32_t listingTier, std::size_t& reclaimed)
    {
        reclaimed = 0;
        bool const enabled = sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.StaticSeller.ReclaimExpiredMail.Enable", false);
        bool const relist = sConfigMgr->GetOption<bool>(
            "LivingAzeroth.Economy.StaticSeller.RelistOnExpire.Enable", false);
        if (!enabled)
        {
            if (relist)
            {
                Fail("relist_requires_expired_mail_reclaim");
                return false;
            }
            return true;
        }

        std::uint32_t const maxPerRun = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzeroth.Economy.StaticSeller.ReclaimExpiredMail.MaxPerRun", 0);
        if (maxPerRun != listingTier)
        {
            Fail("expired_mail_max_per_run_must_equal_listing_tier");
            return false;
        }

        QueryResult result = CharacterDatabase.Query(
            "SELECT m.id,COALESCE(mi.item_guid,0),m.messageType,m.sender,m.receiver,"
            "m.has_items,m.money,m.cod,COALESCE(ii.itemEntry,0),COALESCE(ii.count,0),"
            "COALESCE(ii.owner_guid,0),"
            "(SELECT COUNT(*) FROM mail_items x WHERE x.mail_id=m.id),"
            "(SELECT COUNT(*) FROM auctionhouse a WHERE a.itemguid=mi.item_guid),"
            "(SELECT COUNT(*) FROM character_inventory ci WHERE ci.item=mi.item_guid),"
            "COALESCE(m.subject,'') "
            "FROM mail m LEFT JOIN mail_items mi ON mi.mail_id=m.id "
            "LEFT JOIN item_instance ii ON ii.guid=mi.item_guid "
            "WHERE m.receiver={} AND m.has_items<>0 ORDER BY m.id,mi.item_guid",
            sellerGuid);

        std::vector<ExpiredMailObservation> observed;
        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                ExpiredMailObservation row;
                row.mailId = fields[0].Get<std::uint32_t>();
                row.itemGuid = fields[1].Get<std::uint32_t>();
                row.messageType = fields[2].Get<std::uint8_t>();
                row.market = fields[3].Get<std::uint8_t>();
                row.receiverGuid = fields[4].Get<std::uint32_t>();
                row.hasItems = fields[5].Get<std::uint8_t>() != 0;
                row.money = fields[6].Get<std::uint32_t>();
                row.cod = fields[7].Get<std::uint32_t>();
                row.itemEntry = fields[8].Get<std::uint32_t>();
                row.stack = fields[9].Get<std::uint32_t>();
                row.ownerGuid = fields[10].Get<std::uint32_t>();
                row.mailItemCount = fields[11].Get<std::uint32_t>();
                row.auctionReferences = fields[12].Get<std::uint32_t>();
                row.inventoryReferences = fields[13].Get<std::uint32_t>();

                std::array<std::uint32_t, 5> subjectValues {};
                if (!ParseAuctionMailSubject(
                        fields[14].Get<std::string>(), subjectValues))
                {
                    Fail("expired_mail_subject_parse_failed");
                    return false;
                }
                row.subjectItemEntry = subjectValues[0];
                row.subjectZero = subjectValues[1];
                row.subjectResponse = subjectValues[2];
                row.subjectAuctionId = subjectValues[3];
                row.subjectStack = subjectValues[4];
                observed.push_back(row);
            } while (result->NextRow());
        }

        auto const plan = BuildRotationExpiredMailPlan(
            allowed, observed, sellerGuid, maxPerRun);
        if (!plan.ok)
        {
            Fail(plan.error);
            return false;
        }

        if (plan.cleanup.empty())
            return true;

        CharacterDatabaseTransaction transaction = CharacterDatabase.BeginTransaction();
        for (auto const& row : plan.cleanup)
        {
            CharacterDatabasePreparedStatement* statement =
                CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_ITEM_BY_ID);
            statement->SetData(0, row.mailId);
            transaction->Append(statement);

            statement = CharacterDatabase.GetPreparedStatement(CHAR_DEL_ITEM_INSTANCE);
            statement->SetData(0, row.itemGuid);
            transaction->Append(statement);

            statement = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_BY_ID);
            statement->SetData(0, row.mailId);
            transaction->Append(statement);
        }

        if (!AwaitCharacterCommit(std::move(transaction)))
        {
            Fail("expired_mail_cleanup_persistence_failed");
            return false;
        }

        reclaimed = plan.cleanup.size();
        LOG_INFO(
            "module.living_azeroth.economy",
            "Living Azeroth AI4 expired mail cleanup result=PASS reclaimed={} "
            "seller_guid={} max_per_run={}",
            reclaimed,
            sellerGuid,
            maxPerRun);
        return true;
    }

    static std::vector<ObservedAuction> GetObservedAuctions(Player const& seller)
    {
        std::vector<ObservedAuction> observed;
        for (std::uint8_t market : StaticMarkets)
        {
            AuctionHouseObject* house =
                sAuctionMgr->GetAuctionsMapByHouseId(static_cast<AuctionHouseId>(market));
            for (auto const& [auctionId, auction] : house->GetAuctions())
            {
                if (!auction || auction->owner != seller.GetGUID())
                    continue;

                if (!sAuctionMgr->GetAItem(auction->item_guid))
                {
                    observed.push_back({
                        { market, 0, 0, 0, 0 },
                        true,
                        auctionId
                    });
                    continue;
                }

                observed.push_back({
                    {
                        market,
                        auction->item_template,
                        auction->itemCount,
                        auction->startbid,
                        auction->buyout
                    },
                    static_cast<bool>(auction->bidder) || auction->bid != 0,
                    auctionId
                });
            }
        }

        return observed;
    }

    static void CleanupPending(std::vector<PendingAuction>& pending, Player& seller)
    {
        for (PendingAuction& row : pending)
        {
            row.item->RemoveFromUpdateQueueOf(&seller);
            delete row.item;
            delete row.auction;
        }
        pending.clear();
    }

    static bool ResolveRetirements(std::span<std::uint32_t const> auctionIds,
        Player const& seller, std::vector<PendingRetirement>& retirements)
    {
        retirements.clear();
        retirements.reserve(auctionIds.size());
        for (std::uint32_t auctionId : auctionIds)
        {
            AuctionEntry* auction = nullptr;
            AuctionHouseObject* house = nullptr;
            for (std::uint8_t market : StaticMarkets)
            {
                AuctionHouseObject* candidate = sAuctionMgr->GetAuctionsMapByHouseId(
                    static_cast<AuctionHouseId>(market));
                if (AuctionEntry* found = candidate ? candidate->GetAuction(auctionId) : nullptr)
                {
                    if (auction)
                    {
                        LOG_ERROR(
                            "module.living_azeroth.economy",
                            "Living Azeroth rotation retirement resolution result=FAIL auction={} "
                            "reason=duplicate_house_resolution",
                            auctionId);
                        return false;
                    }
                    auction = found;
                    house = candidate;
                }
            }
            Item* item = auction ? sAuctionMgr->GetAItem(auction->item_guid) : nullptr;
            if (!auction || !house || !item || auction->owner != seller.GetGUID() ||
                static_cast<bool>(auction->bidder) || auction->bid != 0 ||
                item->GetGUID() != auction->item_guid ||
                (item->GetOwnerGUID() &&
                    item->GetOwnerGUID() != seller.GetGUID()))
            {
                LOG_ERROR(
                    "module.living_azeroth.economy",
                    "Living Azeroth rotation retirement resolution result=FAIL auction={} "
                    "auction_found={} house_found={} item_found={} owner_match={} unclaimed={} "
                    "item_guid_match={} item_owner_match={}",
                    auctionId,
                    auction != nullptr,
                    house != nullptr,
                    item != nullptr,
                    auction && auction->owner == seller.GetGUID(),
                    auction && !auction->bidder && auction->bid == 0,
                    auction && item && item->GetGUID() == auction->item_guid,
                    item && (!item->GetOwnerGUID() ||
                        item->GetOwnerGUID() == seller.GetGUID()));
                retirements.clear();
                return false;
            }
            retirements.push_back({ item, auction, house });
        }
        return true;
    }

    static void ReconcileWithSeller(Player& seller, RotationPlan const& rotation,
        std::uint64_t rotationEpoch, std::size_t reclaimedMail,
        std::size_t maxLiveListings)
    {
        std::vector<ObservedAuction> observed = GetObservedAuctions(seller);
        auto plan = BuildRotationReconcilePlan(rotation.desired, observed);
        if (!plan.ok)
        {
            Fail(plan.error);
            return;
        }
        std::size_t const targetDeferredCreates = ApplyDesiredLiveListingTarget(
            plan, observed.size());
        std::size_t const ceilingDeferredCreates = ApplyLiveListingCeiling(
            plan, observed.size(), maxLiveListings);
        std::size_t const deferredCreates =
            targetDeferredCreates + ceilingDeferredCreates;
        if (!plan.ok)
        {
            Fail(plan.error);
            return;
        }

        std::vector<PendingRetirement> retirements;
        if (!ResolveRetirements(plan.retires, seller, retirements))
        {
            Fail("rotation_retirement_resolution_failed");
            return;
        }

        if (plan.creates.empty() && retirements.empty())
        {
            LOG_INFO(
                "module.living_azeroth.economy",
                "Living Azeroth AI4 static seller reconciliation result=PASS retires=0 creates=0 existing={} "
                "desired={} observed={} deferred={} ceiling={} reclaimed_mail={} catalog_sha256={} "
                "epoch={} selected={} alliance={} horde={} neutral={}",
                plan.existingCount,
                plan.desiredCount,
                observed.size(),
                deferredCreates,
                maxLiveListings,
                reclaimedMail,
                FullPriceCatalogSha256,
                rotationEpoch, rotation.selectedItems, rotation.allianceListings,
                rotation.hordeListings, rotation.neutralListings);
            return;
        }

        std::map<BusinessKey, std::size_t> desiredPositions;
        for (std::size_t index = 0; index < rotation.desired.size(); ++index)
            desiredPositions.emplace(rotation.desired[index], index);

        std::vector<PendingAuction> pending;
        pending.reserve(plan.creates.size());
        for (BusinessKey const& key : plan.creates)
        {
            auto const position = desiredPositions.find(key);
            if (position == desiredPositions.end())
            {
                CleanupPending(pending, seller);
                Fail("rotation_position_missing");
                return;
            }
            std::uint32_t const durationSeconds =
                RotationExpirationSeconds(position->second);
            Item* item = Item::CreateItem(key.itemEntry, key.stack, &seller, false, 0, false);
            if (!item)
            {
                CleanupPending(pending, seller);
                Fail("item_creation_failed");
                return;
            }
            item->AddToUpdateQueueOf(&seller);

            AuctionHouseId const houseId = static_cast<AuctionHouseId>(key.market);
            AuctionHouseEntry const* houseEntry = AuctionHouseMgr::GetAuctionHouseEntryFromHouse(houseId);
            AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(houseId);
            if (!houseEntry || !house)
            {
                item->RemoveFromUpdateQueueOf(&seller);
                delete item;
                CleanupPending(pending, seller);
                Fail("auction_house_resolution_failed");
                return;
            }

            AuctionEntry* auction = new AuctionEntry();
            auction->Id = sObjectMgr->GenerateAuctionID();
            auction->houseId = houseId;
            auction->item_guid = item->GetGUID();
            auction->item_template = key.itemEntry;
            auction->itemCount = key.stack;
            auction->owner = seller.GetGUID();
            auction->startbid = key.startBid;
            auction->bid = 0;
            auction->buyout = key.buyout;
            auction->expire_time =
                GameTime::GetGameTime().count() + durationSeconds;
            auction->bidder = ObjectGuid::Empty;
            auction->deposit = AuctionHouseMgr::GetAuctionDeposit(
                houseEntry,
                durationSeconds,
                item,
                key.stack);
            auction->auctionHouseEntry = houseEntry;
            pending.push_back({ item, auction, house });
        }

        CharacterDatabaseTransaction transaction = CharacterDatabase.BeginTransaction();
        for (PendingRetirement& row : retirements)
        {
            row.auction->DeleteFromDB(transaction);
            row.item->DeleteFromDB(transaction);
        }
        for (PendingAuction& row : pending)
        {
            row.item->SaveToDB(transaction);
            row.item->RemoveFromUpdateQueueOf(&seller);
            row.auction->SaveToDB(transaction);
        }
        if (!AwaitCharacterCommit(std::move(transaction)))
        {
            CleanupPending(pending, seller);
            Fail("static_auction_persistence_failed");
            return;
        }

        for (PendingRetirement& row : retirements)
        {
            ObjectGuid const itemGuid = row.item->GetGUID();
            sAuctionMgr->RemoveAItem(itemGuid);
            row.house->RemoveAuction(row.auction);
            delete row.item;
        }
        for (PendingAuction& row : pending)
        {
            sAuctionMgr->AddAItem(row.item);
            row.house->AddAuction(row.auction);
        }

        LOG_INFO(
            "module.living_azeroth.economy",
            "Living Azeroth AI4 static seller reconciliation result=PASS retires={} creates={} existing={} "
            "desired={} observed={} deferred={} ceiling={} reclaimed_mail={} catalog_sha256={} "
            "epoch={} selected={} alliance={} horde={} neutral={}",
            retirements.size(),
            pending.size(),
            plan.existingCount,
            plan.desiredCount,
            observed.size(),
            deferredCreates,
            maxLiveListings,
            reclaimedMail,
            FullPriceCatalogSha256,
            rotationEpoch, rotation.selectedItems, rotation.allianceListings,
            rotation.hordeListings, rotation.neutralListings);
    }
};

class LivingAzerothEconomyAuctionScript final : public AuctionHouseScript
{
public:
    LivingAzerothEconomyAuctionScript() :
        AuctionHouseScript(
            "LivingAzerothEconomyAuctionScript",
            {
                AUCTIONHOUSEHOOK_ON_AUCTION_SUCCESSFUL,
                AUCTIONHOUSEHOOK_ON_AUCTION_EXPIRE,
                AUCTIONHOUSEHOOK_ON_BEFORE_AUCTIONHOUSEMGR_UPDATE
            })
    {
    }

    void OnAuctionExpire(AuctionHouseObject*, AuctionEntry* auction) override
    {
        LivingAzerothEconomyController::Instance().RequestReconcileForStaticAuction(auction);
    }

    void OnAuctionSuccessful(AuctionHouseObject*, AuctionEntry* auction) override
    {
        LivingAzerothEconomyController::Instance().RequestReconcileForStaticAuction(auction);
    }

    void OnBeforeAuctionHouseMgrUpdate() override
    {
        LivingAzerothEconomyController::Instance().ReconcileIfRequested();
    }
};

class LivingAzerothEconomyWorldScript final : public WorldScript
{
public:
    LivingAzerothEconomyWorldScript() :
        WorldScript(
            "LivingAzerothEconomyWorldScript",
            { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE })
    {
    }

    void OnAfterConfigLoad(bool) override
    {
        LivingAzerothEconomyController::Instance().OnConfigLoad();
    }

    void OnStartup() override
    {
        LivingAzerothEconomyController::Instance().ReconcileIfRequested();
    }

    void OnUpdate(std::uint32_t) override
    {
        LivingAzerothEconomyController::Instance().ReconcileIfRequested();
    }
};
}

void AddLivingAzerothEconomyScripts()
{
    new LivingAzerothEconomyAuctionScript();
    new LivingAzerothEconomyWorldScript();
}
