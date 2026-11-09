-- Living Azeroth LA13: bind procurement execution to the validated full price index.
-- The guarded predicate prevents an unknown or manually changed schema identity
-- from being silently promoted.
UPDATE `la_guild_procurement_schema`
SET
    `schema_version` = 3,
    `catalog_sha256` = '5E15BE2C976EE436B1B4BF520154EE9B565007045FF02441C79ADDF8A6CDBC77'
WHERE `singleton_id` = 1
  AND `schema_version` = 2
  AND `catalog_sha256` = 'E442EC7840045A1271B1B350EFC883B6B5C4AB4E2A4FBD75C85C3B28B6752BBD';
