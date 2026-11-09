-- Living Azeroth LA12 expanded procurement catalog identity.
-- Exact v1-to-v2 migration; incompatible identities are not overwritten.

UPDATE `la_guild_procurement_schema`
SET
    `schema_version` = 2,
    `catalog_sha256` = 'E442EC7840045A1271B1B350EFC883B6B5C4AB4E2A4FBD75C85C3B28B6752BBD'
WHERE
    `singleton_id` = 1 AND
    `schema_version` = 1 AND
    `catalog_sha256` = '44D485059201BACB1F246C7C438BC17972729E5ECD6BE734154F3A1CBE068127';