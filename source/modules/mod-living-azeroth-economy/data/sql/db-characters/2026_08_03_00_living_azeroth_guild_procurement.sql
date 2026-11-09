-- Living Azeroth LA11 persistent guild-procurement executor ledger.
-- Module-owned, create-only schema. Live execution remains gated OFF.

CREATE TABLE IF NOT EXISTS `la_guild_procurement_schema` (
    `singleton_id` TINYINT UNSIGNED NOT NULL,
    `schema_version` INT UNSIGNED NOT NULL,
    `catalog_sha256` CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `installed_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`singleton_id`),
    CONSTRAINT `chk_la_guild_procurement_singleton` CHECK (`singleton_id` = 1)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

INSERT INTO `la_guild_procurement_schema`
    (`singleton_id`,`schema_version`,`catalog_sha256`)
VALUES
    (1,1,'44D485059201BACB1F246C7C438BC17972729E5ECD6BE734154F3A1CBE068127')
ON DUPLICATE KEY UPDATE `singleton_id` = `singleton_id`;

CREATE TABLE IF NOT EXISTS `la_guild_procurement_purchase` (
    `action_key` VARCHAR(96) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `auction_id` INT UNSIGNED NOT NULL,
    `status` ENUM('dispatched','verified','rejected') CHARACTER SET ascii NOT NULL,
    `faction` ENUM('alliance','horde') CHARACTER SET ascii NOT NULL,
    `buyer_guid` INT UNSIGNED NOT NULL,
    `buyer_account` INT UNSIGNED NOT NULL,
    `seller_guid` INT UNSIGNED NOT NULL,
    `seller_account` INT UNSIGNED NOT NULL,
    `house_id` TINYINT UNSIGNED NOT NULL,
    `item_guid` INT UNSIGNED NOT NULL,
    `item_entry` INT UNSIGNED NOT NULL,
    `item_count` INT UNSIGNED NOT NULL,
    `buyout_copper` BIGINT UNSIGNED NOT NULL,
    `reference_lot_copper` BIGINT UNSIGNED NOT NULL,
    `ceiling_copper` BIGINT UNSIGNED NOT NULL,
    `deposit_copper` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    `cut_copper` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    `wallet_before_copper` BIGINT UNSIGNED NOT NULL,
    `buyer_debit_copper` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    `seller_mail_copper` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    `settlement_reason` VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NULL,
    `dispatched_at_unix` BIGINT UNSIGNED NOT NULL,
    `finalized_at_unix` BIGINT UNSIGNED NULL,
    `dispatch_evidence_sha256` CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `settlement_evidence_sha256` CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NULL,
    PRIMARY KEY (`action_key`),
    UNIQUE KEY `uq_la_guild_procurement_auction` (`auction_id`),
    KEY `idx_la_guild_procurement_status_time` (`status`,`dispatched_at_unix`),
    KEY `idx_la_guild_procurement_seller_time` (`seller_guid`,`dispatched_at_unix`),
    CONSTRAINT `chk_la_guild_procurement_identity` CHECK
        (`buyer_guid` > 0 AND `buyer_account` > 0 AND `seller_guid` > 0 AND
         `seller_account` > 0 AND `buyer_account` <> `seller_account`),
    CONSTRAINT `chk_la_guild_procurement_item` CHECK
        (`item_guid` > 0 AND `item_entry` > 0 AND `item_count` > 0),
    CONSTRAINT `chk_la_guild_procurement_price` CHECK
        (`buyout_copper` > 0 AND `reference_lot_copper` > 0 AND
         `ceiling_copper` >= `buyout_copper`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `la_guild_procurement_faction_daily` (
    `faction` ENUM('alliance','horde') CHARACTER SET ascii NOT NULL,
    `utc_day` INT UNSIGNED NOT NULL,
    `dispatch_count` INT UNSIGNED NOT NULL DEFAULT 0,
    `dispatch_spend_copper` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`faction`,`utc_day`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `la_guild_procurement_seller_daily` (
    `faction` ENUM('alliance','horde') CHARACTER SET ascii NOT NULL,
    `seller_guid` INT UNSIGNED NOT NULL,
    `utc_day` INT UNSIGNED NOT NULL,
    `dispatch_count` INT UNSIGNED NOT NULL DEFAULT 0,
    `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`faction`,`seller_guid`,`utc_day`),
    CONSTRAINT `chk_la_guild_procurement_seller` CHECK (`seller_guid` > 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
