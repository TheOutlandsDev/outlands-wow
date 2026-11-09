-- Living Azeroth LA7 persistent profile-commerce ledger.
-- Module-owned, create-only schema. All live application remains gated OFF.

CREATE TABLE IF NOT EXISTS `la_profile_commerce_schema` (
    `singleton_id` TINYINT UNSIGNED NOT NULL,
    `schema_version` INT UNSIGNED NOT NULL,
    `catalog_sha256` CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `installed_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`singleton_id`),
    CONSTRAINT `chk_la_profile_commerce_singleton` CHECK (`singleton_id` = 1)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

INSERT INTO `la_profile_commerce_schema`
    (`singleton_id`,`schema_version`,`catalog_sha256`)
VALUES
    (1,1,'0CE2FC5B5D057FC3089F5668019FD5541F3BD8EF4CC6FB3C83844FC5C2F0E7B7')
ON DUPLICATE KEY UPDATE
    `schema_version` = 1,
    `catalog_sha256` = '0CE2FC5B5D057FC3089F5668019FD5541F3BD8EF4CC6FB3C83844FC5C2F0E7B7';

CREATE TABLE IF NOT EXISTS `la_profile_commerce_ledger` (
    `ledger_id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `action_key` VARCHAR(96) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `phase` ENUM('dispatched','verified','rejected') CHARACTER SET ascii NOT NULL,
    `action_kind` ENUM('auction_list','auction_buyout','auction_mail_reclaim')
        CHARACTER SET ascii NOT NULL,
    `character_guid` INT UNSIGNED NOT NULL,
    `account_id` INT UNSIGNED NOT NULL,
    `seller_guid` INT UNSIGNED NOT NULL,
    `house_id` TINYINT UNSIGNED NOT NULL,
    `target_id` INT UNSIGNED NOT NULL,
    `item_guid` INT UNSIGNED NOT NULL,
    `item_entry` INT UNSIGNED NOT NULL,
    `item_count` INT UNSIGNED NOT NULL,
    `total_price_copper` BIGINT UNSIGNED NOT NULL,
    `deposit_copper` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    `cut_copper` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    `wallet_before_copper` BIGINT UNSIGNED NOT NULL,
    `recorded_at_unix` BIGINT UNSIGNED NOT NULL,
    `evidence_sha256` CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NULL,
    PRIMARY KEY (`ledger_id`),
    UNIQUE KEY `uq_la_profile_commerce_action_phase` (`action_key`,`phase`),
    KEY `idx_la_profile_commerce_character_time` (`character_guid`,`recorded_at_unix`),
    KEY `idx_la_profile_commerce_target` (`action_kind`,`target_id`),
    CONSTRAINT `chk_la_profile_commerce_identity` CHECK
        (`character_guid` > 0 AND `account_id` > 0 AND `seller_guid` > 0),
    CONSTRAINT `chk_la_profile_commerce_item` CHECK (`item_guid` > 0 AND `item_entry` > 0 AND `item_count` > 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `la_profile_commerce_daily_usage` (
    `character_guid` INT UNSIGNED NOT NULL,
    `utc_day` INT UNSIGNED NOT NULL,
    `dispatch_count` INT UNSIGNED NOT NULL DEFAULT 0,
    `dispatch_spend_copper` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`character_guid`,`utc_day`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `la_profile_commerce_settlement` (
    `settlement_id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `action_key` VARCHAR(96) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `outcome` ENUM('verified','rejected') CHARACTER SET ascii NOT NULL,
    `auction_id` INT UNSIGNED NOT NULL,
    `item_guid` INT UNSIGNED NOT NULL,
    `buyer_debit_copper` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    `seller_mail_copper` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    `deposit_copper` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    `cut_copper` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    `evidence_sha256` CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `recorded_at_unix` BIGINT UNSIGNED NOT NULL,
    PRIMARY KEY (`settlement_id`),
    UNIQUE KEY `uq_la_profile_commerce_settlement_action` (`action_key`,`outcome`),
    KEY `idx_la_profile_commerce_settlement_auction` (`auction_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
