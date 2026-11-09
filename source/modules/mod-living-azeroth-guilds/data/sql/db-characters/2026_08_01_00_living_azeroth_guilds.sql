-- Living Azeroth faction-guild ownership and persistent ranking projection.
-- This migration owns only la_guild_* objects. It does not mutate native guild,
-- character, inventory, mail, auction, or playerbots rows.

CREATE TABLE IF NOT EXISTS `la_guild_schema` (
    `singleton_id` TINYINT UNSIGNED NOT NULL,
    `schema_version` INT UNSIGNED NOT NULL,
    `installed_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`singleton_id`),
    CONSTRAINT `chk_la_guild_schema_singleton` CHECK (`singleton_id` = 1)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

INSERT INTO `la_guild_schema` (`singleton_id`, `schema_version`)
VALUES (1, 1)
ON DUPLICATE KEY UPDATE `schema_version` = GREATEST(`schema_version`, VALUES(`schema_version`));

CREATE TABLE IF NOT EXISTS `la_faction_guild` (
    `team` TINYINT UNSIGNED NOT NULL,
    `guild_id` INT UNSIGNED NULL,
    `expected_name` VARCHAR(24) NOT NULL,
    `leader_guid` INT UNSIGNED NULL,
    `state` ENUM('declared','ready','blocked','rollback') CHARACTER SET ascii NOT NULL DEFAULT 'declared',
    `policy_version` INT UNSIGNED NOT NULL DEFAULT 1,
    `revision` BIGINT UNSIGNED NOT NULL DEFAULT 1,
    `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`team`),
    UNIQUE KEY `uq_la_faction_guild_id` (`guild_id`),
    UNIQUE KEY `uq_la_faction_guild_name` (`expected_name`),
    CONSTRAINT `chk_la_faction_guild_team` CHECK (`team` IN (0,1))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

INSERT INTO `la_faction_guild` (`team`, `expected_name`)
VALUES (0, 'Living Azeroth Alliance'), (1, 'Living Azeroth Horde')
ON DUPLICATE KEY UPDATE `expected_name` = VALUES(`expected_name`);

CREATE TABLE IF NOT EXISTS `la_guild_rank_policy` (
    `policy_version` INT UNSIGNED NOT NULL,
    `native_rank` TINYINT UNSIGNED NOT NULL,
    `alliance_name` VARCHAR(20) NOT NULL,
    `horde_name` VARCHAR(20) NOT NULL,
    `minimum_points` INT UNSIGNED NOT NULL,
    `rights_mask` INT UNSIGNED NOT NULL,
    `bank_money_per_day` INT UNSIGNED NOT NULL DEFAULT 0,
    `accepted` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`policy_version`, `native_rank`),
    CONSTRAINT `chk_la_guild_rank_id` CHECK (`native_rank` <= 9),
    CONSTRAINT `chk_la_guild_rank_accepted` CHECK (`accepted` IN (0,1))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

INSERT INTO `la_guild_rank_policy`
(`policy_version`,`native_rank`,`alliance_name`,`horde_name`,`minimum_points`,`rights_mask`,`bank_money_per_day`,`accepted`)
VALUES
(1,0,'High Commander','High Warlord',4294967295,1962495,0,0),
(1,1,'War Council','War Council',25000,57423,0,0),
(1,2,'General','General',18000,57423,0,0),
(1,3,'Colonel','Colonel',13000,57423,0,0),
(1,4,'Major','Major',9500,57423,0,0),
(1,5,'Captain','Captain',6500,8259,0,0),
(1,6,'Lieutenant','Lieutenant',4000,8259,0,0),
(1,7,'Sergeant','Sergeant',2000,8259,0,0),
(1,8,'Veteran','Veteran',1000,67,0,0),
(1,9,'Recruit','Recruit',0,67,0,0)
ON DUPLICATE KEY UPDATE
`alliance_name`=VALUES(`alliance_name`),`horde_name`=VALUES(`horde_name`),
`minimum_points`=VALUES(`minimum_points`),`rights_mask`=VALUES(`rights_mask`),
`bank_money_per_day`=VALUES(`bank_money_per_day`);

CREATE TABLE IF NOT EXISTS `la_guild_exclusion` (
    `character_guid` INT UNSIGNED NOT NULL,
    `reason` VARCHAR(64) CHARACTER SET ascii NOT NULL,
    `active` TINYINT UNSIGNED NOT NULL DEFAULT 1,
    `declared_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`character_guid`),
    CONSTRAINT `chk_la_guild_exclusion_active` CHECK (`active` IN (0,1))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- Server-owned economy seller. This is an idempotent, explicit exclusion from
-- faction membership and ranking; no core character row is changed.
INSERT INTO `la_guild_exclusion` (`character_guid`,`reason`,`active`)
VALUES (900001,'system_economy_seller',1)
ON DUPLICATE KEY UPDATE `reason`=VALUES(`reason`),`active`=VALUES(`active`);

CREATE TABLE IF NOT EXISTS `la_guild_character` (
    `character_guid` INT UNSIGNED NOT NULL,
    `subject_kind` ENUM('human','ai','system','excluded','quarantined') CHARACTER SET ascii NOT NULL,
    `native_team` TINYINT UNSIGNED NOT NULL,
    `expected_guild_id` INT UNSIGNED NULL,
    `observed_guild_id` INT UNSIGNED NULL,
    `observed_native_rank` TINYINT UNSIGNED NULL,
    `projected_native_rank` TINYINT UNSIGNED NOT NULL DEFAULT 9,
    `level_snapshot` TINYINT UNSIGNED NOT NULL,
    `rewarded_quests_snapshot` INT UNSIGNED NOT NULL,
    `honorable_kills_snapshot` INT UNSIGNED NOT NULL,
    `rank_points` INT UNSIGNED NOT NULL,
    `enrollment_state` ENUM('pending','ready','protected','excluded','blocked') CHARACTER SET ascii NOT NULL DEFAULT 'pending',
    `policy_version` INT UNSIGNED NOT NULL DEFAULT 1,
    `revision` BIGINT UNSIGNED NOT NULL DEFAULT 1,
    `last_audited_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`character_guid`),
    KEY `idx_la_guild_character_team_rank` (`native_team`,`rank_points`,`character_guid`),
    KEY `idx_la_guild_character_state` (`enrollment_state`,`native_team`),
    CONSTRAINT `chk_la_guild_character_team` CHECK (`native_team` IN (0,1,255)),
    CONSTRAINT `chk_la_guild_character_rank` CHECK (`projected_native_rank` BETWEEN 1 AND 9)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `la_guild_service_event` (
    `event_id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `source_event_key` VARCHAR(128) CHARACTER SET ascii NOT NULL,
    `character_guid` INT UNSIGNED NOT NULL,
    `native_team` TINYINT UNSIGNED NOT NULL,
    `category` VARCHAR(32) CHARACTER SET ascii NOT NULL,
    `reason` VARCHAR(64) CHARACTER SET ascii NOT NULL,
    `points` INT NOT NULL,
    `evidence_sha256` CHAR(64) CHARACTER SET ascii NULL,
    `reverses_event_id` BIGINT UNSIGNED NULL,
    `committed_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`event_id`),
    UNIQUE KEY `uq_la_guild_service_source` (`source_event_key`),
    UNIQUE KEY `uq_la_guild_service_reversal` (`reverses_event_id`),
    KEY `idx_la_guild_service_character` (`character_guid`,`committed_at`),
    CONSTRAINT `chk_la_guild_service_team` CHECK (`native_team` IN (0,1)),
    CONSTRAINT `chk_la_guild_service_points` CHECK (`points` BETWEEN -1000 AND 1000)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `la_guild_decision` (
    `decision_id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `decision_key` CHAR(64) CHARACTER SET ascii NOT NULL,
    `character_guid` INT UNSIGNED NOT NULL,
    `decision_kind` ENUM('enrollment','rank','exclusion','recovery') CHARACTER SET ascii NOT NULL,
    `observed_guild_id` INT UNSIGNED NULL,
    `proposed_guild_id` INT UNSIGNED NULL,
    `observed_rank` TINYINT UNSIGNED NULL,
    `proposed_rank` TINYINT UNSIGNED NULL,
    `status` ENUM('observed','applied','rejected','blocked','rolled_back') CHARACTER SET ascii NOT NULL,
    `reason` VARCHAR(64) CHARACTER SET ascii NOT NULL,
    `policy_version` INT UNSIGNED NOT NULL,
    `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`decision_id`),
    UNIQUE KEY `uq_la_guild_decision_key` (`decision_key`),
    KEY `idx_la_guild_decision_character` (`character_guid`,`created_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE OR REPLACE VIEW `la_guild_rankings` AS
SELECT
    `character_guid`, `subject_kind`, `native_team`, `rank_points`,
    `projected_native_rank`, `enrollment_state`,
    DENSE_RANK() OVER (
        PARTITION BY `native_team`
        ORDER BY `rank_points` DESC, `character_guid` ASC
    ) AS `faction_position`
FROM `la_guild_character`
WHERE `subject_kind` NOT IN ('excluded','quarantined');
