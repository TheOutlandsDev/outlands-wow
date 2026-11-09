#ifndef LIVING_AZEROTH_AUTOMATED_LISTING_BRIDGE_H
#define LIVING_AZEROTH_AUTOMATED_LISTING_BRIDGE_H

#include <cstdint>
#include <string>

namespace LivingAzeroth::Economy::AutomatedListing
{
enum class ReceiptState : std::uint8_t { Missing, Applied, Conflict, Unavailable };

struct Pending
{
    std::string ActionId;
    std::string EnvironmentId;
    std::uint32_t RealmId = 0;
    std::string PolicyVersion;
    std::string PayloadSha256;
    std::uint32_t SellerGuid = 0;
    std::uint32_t SellerAccount = 0;
    std::uint32_t SourceItemGuid = 0;
    std::uint32_t ItemEntry = 0;
    std::uint32_t ItemCount = 0;
    std::uint32_t HouseId = 0;
    std::uint32_t StartBid = 0;
    std::uint32_t Buyout = 0;
    std::uint32_t Deposit = 0;
    std::uint32_t DurationSeconds = 0;
};

bool Ready();
bool Register(Pending const& pending);
void Forget(std::uint32_t sellerGuid, std::uint32_t sourceItemGuid);
ReceiptState ReadReceipt(Pending const& pending, std::string* resultSha256 = nullptr);
bool IsAutomationResaleBlocked(std::uint32_t itemGuid);
}

void AddLivingAzerothAutomatedListingBridgeScripts();

#endif
