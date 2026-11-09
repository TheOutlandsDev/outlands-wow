-- Living Azeroth AI-3: observe-only human profile snapshots and decision ledger.
--
-- Forward migration only. Gate tooling must prove all four names are absent
-- before applying this file to an isolated restored Character database.
-- No DROP, DELETE, UPDATE, or core-table DML belongs in this migration.

CREATE TABLE `la_profile_snapshot` (
    `snapshot_id` CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `captured_at_unix_ms` BIGINT UNSIGNED NOT NULL,
    `provenance` ENUM('online','last_known','baseline') CHARACTER SET ascii NOT NULL,
    `policy_version` VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `config_sha256` CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `input_sha256` CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `online_human_count` INT UNSIGNED NOT NULL,
    `policy_input_count` INT UNSIGNED NOT NULL,
    `primary_character_guid` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    `population_requested_target` INT UNSIGNED NOT NULL,
    `population_bounded_target` INT UNSIGNED NOT NULL,
    `observe_only` TINYINT UNSIGNED NOT NULL DEFAULT 1,
    PRIMARY KEY (`snapshot_id`),
    KEY `idx_la_profile_snapshot_time` (`captured_at_unix_ms`),
    KEY `idx_la_profile_snapshot_provenance` (`provenance`),
    CONSTRAINT `chk_la_snapshot_observe_only` CHECK (`observe_only` = 1),
    CONSTRAINT `chk_la_snapshot_population_cap`
        CHECK (`population_bounded_target` <= 50),
    CONSTRAINT `chk_la_snapshot_target_order`
        CHECK (`population_bounded_target` <= `population_requested_target`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `la_profile_snapshot_member` (
    `snapshot_id` CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `ordinal` SMALLINT UNSIGNED NOT NULL,
    `input_kind` ENUM('online_human','reference_profile') CHARACTER SET ascii NOT NULL,
    `character_guid` BIGINT UNSIGNED NOT NULL,
    `faction` TINYINT UNSIGNED NOT NULL,
    `class_id` TINYINT UNSIGNED NOT NULL,
    `level` TINYINT UNSIGNED NOT NULL,
    `map_id` INT UNSIGNED NOT NULL,
    `zone_id` INT UNSIGNED NOT NULL,
    `grouped` TINYINT UNSIGNED NOT NULL,
    `in_instance` TINYINT UNSIGNED NOT NULL,
    `average_item_level_x100` INT UNSIGNED NOT NULL,
    `equipped_item_count` TINYINT UNSIGNED NOT NULL,
    `active_quest_count` TINYINT UNSIGNED NOT NULL,
    `rewarded_quest_count` INT UNSIGNED NOT NULL,
    `profession1_skill_id` SMALLINT UNSIGNED NOT NULL,
    `profession1_value` SMALLINT UNSIGNED NOT NULL,
    `profession1_step` TINYINT UNSIGNED NOT NULL,
    `profession2_skill_id` SMALLINT UNSIGNED NOT NULL,
    `profession2_value` SMALLINT UNSIGNED NOT NULL,
    `profession2_step` TINYINT UNSIGNED NOT NULL,
    PRIMARY KEY (`snapshot_id`, `ordinal`),
    KEY `idx_la_snapshot_member_guid` (`character_guid`),
    CONSTRAINT `fk_la_snapshot_member_snapshot`
        FOREIGN KEY (`snapshot_id`) REFERENCES `la_profile_snapshot` (`snapshot_id`),
    CONSTRAINT `chk_la_snapshot_member_faction`
        CHECK (`faction` IN (0,1,255)),
    CONSTRAINT `chk_la_snapshot_member_class`
        CHECK (`class_id` IN (1,2,3,4,5,6,7,8,9,11)),
    CONSTRAINT `chk_la_snapshot_member_level`
        CHECK (`level` BETWEEN 1 AND 80),
    CONSTRAINT `chk_la_snapshot_member_grouped`
        CHECK (`grouped` IN (0,1)),
    CONSTRAINT `chk_la_snapshot_member_instance`
        CHECK (`in_instance` IN (0,1)),
    CONSTRAINT `chk_la_snapshot_member_equipped`
        CHECK (`equipped_item_count` <= 19),
    CONSTRAINT `chk_la_snapshot_member_active_quests`
        CHECK (`active_quest_count` <= 25),
    CONSTRAINT `chk_la_snapshot_member_profession1_step`
        CHECK (`profession1_step` <= 6),
    CONSTRAINT `chk_la_snapshot_member_profession2_step`
        CHECK (`profession2_step` <= 6)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `la_director_decision` (
    `decision_id` CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `snapshot_id` CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `correlation_id` CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `decided_at_unix_ms` BIGINT UNSIGNED NOT NULL,
    `subsystem` ENUM('population','activity','economy','guild','companion')
        CHARACTER SET ascii NOT NULL,
    `policy_version` VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `action` VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `requested_target` INT UNSIGNED NOT NULL,
    `bounded_target` INT UNSIGNED NOT NULL,
    `applied_target` INT UNSIGNED NOT NULL DEFAULT 0,
    `state` ENUM('observe_only','rejected') CHARACTER SET ascii NOT NULL,
    `reason` VARCHAR(96) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `rate_limit_json` JSON NOT NULL,
    `budget_json` JSON NOT NULL,
    `detail_json` JSON NOT NULL,
    PRIMARY KEY (`decision_id`),
    KEY `idx_la_director_decision_snapshot` (`snapshot_id`),
    KEY `idx_la_director_decision_time` (`decided_at_unix_ms`),
    KEY `idx_la_director_decision_subsystem` (`subsystem`),
    CONSTRAINT `fk_la_director_decision_snapshot`
        FOREIGN KEY (`snapshot_id`) REFERENCES `la_profile_snapshot` (`snapshot_id`),
    CONSTRAINT `chk_la_director_applied_zero` CHECK (`applied_target` = 0),
    CONSTRAINT `chk_la_director_target_order`
        CHECK (`bounded_target` <= `requested_target`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE `la_reference_profile` (
    `character_guid` BIGINT UNSIGNED NOT NULL,
    `faction` TINYINT UNSIGNED NOT NULL,
    `class_id` TINYINT UNSIGNED NOT NULL,
    `level` TINYINT UNSIGNED NOT NULL,
    `map_id` INT UNSIGNED NOT NULL,
    `zone_id` INT UNSIGNED NOT NULL,
    `grouped` TINYINT UNSIGNED NOT NULL,
    `in_instance` TINYINT UNSIGNED NOT NULL,
    `average_item_level_x100` INT UNSIGNED NOT NULL,
    `equipped_item_count` TINYINT UNSIGNED NOT NULL,
    `active_quest_count` TINYINT UNSIGNED NOT NULL,
    `rewarded_quest_count` INT UNSIGNED NOT NULL,
    `profession1_skill_id` SMALLINT UNSIGNED NOT NULL,
    `profession1_value` SMALLINT UNSIGNED NOT NULL,
    `profession1_step` TINYINT UNSIGNED NOT NULL,
    `profession2_skill_id` SMALLINT UNSIGNED NOT NULL,
    `profession2_value` SMALLINT UNSIGNED NOT NULL,
    `profession2_step` TINYINT UNSIGNED NOT NULL,
    `observed_at_unix` BIGINT UNSIGNED NOT NULL,
    `source_snapshot_id` CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    `config_sha256` CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    PRIMARY KEY (`character_guid`),
    KEY `idx_la_reference_observed` (`observed_at_unix`),
    CONSTRAINT `fk_la_reference_snapshot`
        FOREIGN KEY (`source_snapshot_id`) REFERENCES `la_profile_snapshot` (`snapshot_id`),
    CONSTRAINT `chk_la_reference_faction`
        CHECK (`faction` IN (0,1)),
    CONSTRAINT `chk_la_reference_class`
        CHECK (`class_id` IN (1,2,3,4,5,6,7,8,9,11)),
    CONSTRAINT `chk_la_reference_level`
        CHECK (`level` BETWEEN 1 AND 80),
    CONSTRAINT `chk_la_reference_grouped`
        CHECK (`grouped` IN (0,1)),
    CONSTRAINT `chk_la_reference_instance`
        CHECK (`in_instance` IN (0,1)),
    CONSTRAINT `chk_la_reference_equipped`
        CHECK (`equipped_item_count` <= 19),
    CONSTRAINT `chk_la_reference_active_quests`
        CHECK (`active_quest_count` <= 25),
    CONSTRAINT `chk_la_reference_profession1_step`
        CHECK (`profession1_step` <= 6),
    CONSTRAINT `chk_la_reference_profession2_step`
        CHECK (`profession2_step` <= 6)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;


