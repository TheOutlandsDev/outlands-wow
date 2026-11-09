#!/usr/bin/env bash
set -euo pipefail

role="${1:-}"
case "$role" in
    authserver|worldserver) ;;
    *)
        echo "usage: gate3-server-entrypoint authserver|worldserver" >&2
        exit 64
        ;;
esac

secret_file=/run/secrets/acore_password
if [[ ! -r "$secret_file" ]]; then
    echo "database credential file is not readable" >&2
    exit 65
fi

runtime_root=/run/living-azeroth
config_root="$runtime_root/etc"
log_root="$runtime_root/logs"
install_root=/azerothcore/dist
module_root="$install_root/etc/modules"
module_template_root=/opt/living-azeroth-gate3/module-templates
ai_profile_renderer=/usr/local/bin/gate8-ai-profile

mkdir -p "$config_root" "$log_root"
cp "$install_root/etc/authserver.conf.dist" "$config_root/authserver.conf"
cp "$install_root/etc/worldserver.conf.dist" "$config_root/worldserver.conf"

replace_setting() {
    local file="$1"
    local key="$2"
    local value="$3"
    local temporary="$file.tmp"
    awk -v wanted="$key" -v replacement="$key = $value" '
        {
            line = $0
            sub(/\r$/, "", line)
            equals = index(line, "=")
            if (equals > 0) {
                lhs = substr(line, 1, equals - 1)
                gsub(/[[:space:]]/, "", lhs)
                if (lhs == wanted) {
                    print replacement
                    matches += 1
                    next
                }
            }
            print line
        }
        END {
            if (matches != 1)
                exit 42
        }
    ' "$file" > "$temporary"
    mv "$temporary" "$file"
}

acore_password="$(tr -d '\r\n' < "$secret_file")"
if [[ ! "$acore_password" =~ ^[A-Za-z0-9_-]{32,}$ ]]; then
    echo "database credential has an invalid format" >&2
    exit 66
fi

auth_database="database;3306;acore;$acore_password;la_ahfinal11_auth"
characters_database="database;3306;acore;$acore_password;la_ahfinal11_characters"
world_database="database;3306;acore;$acore_password;la_ahfinal11_world"
playerbots_database="database;3306;acore;$acore_password;la_ahfinal11_playerbots"

auth_config="$config_root/authserver.conf"
replace_setting "$auth_config" LogsDir '"/run/living-azeroth/logs"'
replace_setting "$auth_config" RealmServerPort 3724
replace_setting "$auth_config" BindIP '"0.0.0.0"'
replace_setting "$auth_config" LoginDatabaseInfo "\"$auth_database\""
replace_setting "$auth_config" Updates.EnableDatabases 0

world_config="$config_root/worldserver.conf"
replace_setting "$world_config" RealmID 1
replace_setting "$world_config" WorldServerPort 8085
replace_setting "$world_config" BindIP '"0.0.0.0"'
replace_setting "$world_config" LoginDatabaseInfo "\"$auth_database\""
replace_setting "$world_config" WorldDatabaseInfo "\"$world_database\""
replace_setting "$world_config" CharacterDatabaseInfo "\"$characters_database\""
replace_setting "$world_config" DataDir '"/azerothcore/data"'
replace_setting "$world_config" LogsDir '"/run/living-azeroth/logs"'
replace_setting "$world_config" Updates.EnableDatabases 0
replace_setting "$world_config" Console.Enable 0

if [[ "$role" == worldserver ]]; then
    for source in "$module_template_root"/*.conf.dist; do
        name="$(basename "$source" .dist)"
        cp "$source" "$module_root/$name"
    done

    playerbots_config="$module_root/playerbots.conf"
    replace_setting "$playerbots_config" PlayerbotsDatabaseInfo "\"$playerbots_database\""
    replace_setting "$playerbots_config" Playerbots.Updates.EnableDatabases 0
    "$ai_profile_renderer" "$module_root" "${LIVING_AZEROTH_AI_PROFILE:-disabled}" \
        "${LIVING_AZEROTH_AI_BACKEND_PROBE:-0}"
fi

chmod -R go-rwx "$runtime_root"
unset acore_password auth_database characters_database world_database playerbots_database

if [[ "$role" == authserver ]]; then
    exec "$install_root/bin/authserver" --config "$auth_config"
fi
exec "$install_root/bin/worldserver" --config "$world_config"
