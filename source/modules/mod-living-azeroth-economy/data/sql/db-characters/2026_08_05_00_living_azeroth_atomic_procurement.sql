-- Gate 0 destination-side processed-action receipt.
-- The row is written in the same characters transaction as the auction mutation.

CREATE TABLE IF NOT EXISTS `la_outlands_processed_action` (
    `action_id` VARCHAR(96) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `action_type` VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `environment_id` VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `realm_id` INT UNSIGNED NOT NULL,
    `policy_version` VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `payload_sha256` CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `result_sha256` CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `auction_id` INT UNSIGNED NOT NULL,
    `buyer_guid` INT UNSIGNED NOT NULL,
    `seller_guid` INT UNSIGNED NOT NULL,
    `item_guid` INT UNSIGNED NOT NULL,
    `item_entry` INT UNSIGNED NOT NULL,
    `item_count` INT UNSIGNED NOT NULL,
    `buyout_copper` BIGINT UNSIGNED NOT NULL,
    `applied_at_unix` BIGINT UNSIGNED NOT NULL,
    PRIMARY KEY (`action_id`),
    UNIQUE KEY `uq_la_outlands_processed_action_auction` (`action_type`,`auction_id`),
    KEY `idx_la_outlands_processed_action_time` (`applied_at_unix`),
    CONSTRAINT `chk_la_outlands_processed_action_identity` CHECK
        (`auction_id` > 0 AND `buyer_guid` > 0 AND `seller_guid` > 0 AND
         `buyer_guid` <> `seller_guid`),
    CONSTRAINT `chk_la_outlands_processed_action_item` CHECK
        (`item_guid` > 0 AND `item_entry` > 0 AND `item_count` > 0),
    CONSTRAINT `chk_la_outlands_processed_action_price` CHECK (`buyout_copper` > 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `la_outlands_action_audit` (
    `audit_id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `action_id` VARCHAR(96) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `action_type` VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `environment_id` VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `realm_id` INT UNSIGNED NOT NULL,
    `policy_version` VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `payload_sha256` CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `result_sha256` CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `result` VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `recorded_at_unix` BIGINT UNSIGNED NOT NULL,
    PRIMARY KEY (`audit_id`),
    UNIQUE KEY `uq_la_outlands_action_audit_result` (`action_id`,`result`),
    KEY `idx_la_outlands_action_audit_time` (`recorded_at_unix`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
UPDATE `la_guild_procurement_schema`
SET `schema_version` = 4
WHERE `singleton_id` = 1 AND `schema_version` = 3;
