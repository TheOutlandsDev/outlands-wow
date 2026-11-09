-- Additive Gate 0 provenance and automated-listing destination receipts.
-- Rollback is intentionally manual and only after the feature is disabled and
-- receipt/provenance rows have been exported for audit preservation.

CREATE TABLE IF NOT EXISTS `la_outlands_item_provenance` (
    `item_guid` INT UNSIGNED NOT NULL,
    `purchase_action_id` VARCHAR(96) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `source_seller_guid` INT UNSIGNED NOT NULL,
    `receiving_ai_guid` INT UNSIGNED NOT NULL,
    `reason` VARCHAR(48) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `automation_resale_blocked` TINYINT UNSIGNED NOT NULL DEFAULT 1,
    `applied_at_unix` BIGINT UNSIGNED NOT NULL,
    `release_action_id` VARCHAR(96) CHARACTER SET ascii COLLATE ascii_bin NULL,
    `released_at_unix` BIGINT UNSIGNED NULL,
    PRIMARY KEY (`item_guid`),
    UNIQUE KEY `uq_la_outlands_item_provenance_action` (`purchase_action_id`),
    KEY `idx_la_outlands_item_provenance_receiver` (`receiving_ai_guid`),
    CONSTRAINT `chk_la_outlands_item_provenance_identity` CHECK
        (`item_guid` > 0 AND `source_seller_guid` > 0 AND `receiving_ai_guid` > 0),
    CONSTRAINT `chk_la_outlands_item_provenance_block` CHECK
        (`automation_resale_blocked` IN (0,1))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `la_outlands_auction_listing_receipt` (
    `action_id` VARCHAR(96) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `action_type` VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `environment_id` VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `realm_id` INT UNSIGNED NOT NULL,
    `policy_version` VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `payload_sha256` CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `result_sha256` CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `auction_id` INT UNSIGNED NOT NULL,
    `seller_guid` INT UNSIGNED NOT NULL,
    `source_item_guid` INT UNSIGNED NOT NULL,
    `item_entry` INT UNSIGNED NOT NULL,
    `item_count` INT UNSIGNED NOT NULL,
    `house_id` INT UNSIGNED NOT NULL,
    `start_bid_copper` BIGINT UNSIGNED NOT NULL,
    `buyout_copper` BIGINT UNSIGNED NOT NULL,
    `deposit_copper` BIGINT UNSIGNED NOT NULL,
    `duration_seconds` INT UNSIGNED NOT NULL,
    `applied_at_unix` BIGINT UNSIGNED NOT NULL,
    PRIMARY KEY (`action_id`),
    UNIQUE KEY `uq_la_outlands_auction_listing_receipt_auction` (`auction_id`),
    KEY `idx_la_outlands_auction_listing_receipt_seller` (`seller_guid`,`applied_at_unix`),
    CONSTRAINT `chk_la_outlands_auction_listing_receipt_identity` CHECK
        (`auction_id` > 0 AND `seller_guid` > 0 AND `source_item_guid` > 0),
    CONSTRAINT `chk_la_outlands_auction_listing_receipt_item` CHECK
        (`item_entry` > 0 AND `item_count` > 0),
    CONSTRAINT `chk_la_outlands_auction_listing_receipt_price` CHECK
        (`start_bid_copper` > 0 AND `buyout_copper` >= `start_bid_copper`),
    CONSTRAINT `chk_la_outlands_auction_listing_receipt_duration` CHECK
        (`duration_seconds` > 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

UPDATE `la_guild_procurement_schema`
SET `schema_version` = 5
WHERE `singleton_id` = 1 AND `schema_version` = 4;
