#!/bin/sh

set -e

. /usr/share/openmediavault/scripts/helper-functions

xpath="/config/services/timeshift"

if ! omv_config_exists "${xpath}"; then
  omv_config_add_node "/config/services" "timeshift"
  omv_config_add_key "${xpath}" "backupdev" ""
  omv_config_add_key "${xpath}" "stopcronemail" "0"
  omv_config_add_key "${xpath}" "monthly" "0"
  omv_config_add_key "${xpath}" "weekly" "0"
  omv_config_add_key "${xpath}" "daily" "0"
  omv_config_add_key "${xpath}" "hourly" "0"
  omv_config_add_key "${xpath}" "boot" "0"
  omv_config_add_key "${xpath}" "exclude" "/srv/**,/home/**,/root/**"
fi

exit 0
