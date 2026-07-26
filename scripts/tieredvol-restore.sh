#!/bin/bash
# TieredVol — Restore volumes from saved config after reboot
# Reads /etc/tieredvol/*.conf and rebuilds dm-linear + LVM striped volumes.
#
# Usage: tieredvol-restore.sh [--dry-run]
#   --dry-run   Show what would be done without making changes

set -euo pipefail

CONF_DIR="/etc/tieredvol"
LOG_TAG="tieredvol-restore"
DRY_RUN=0

if [[ "${1:-}" == "--dry-run" ]]; then
    DRY_RUN=1
fi

log() { echo "[$LOG_TAG] $*"; }
err() { echo "[$LOG_TAG] ERROR: $*" >&2; }

run() {
    if [[ $DRY_RUN -eq 1 ]]; then
        log "DRY-RUN: $*"
        return 0
    fi
    "$@"
}

parse_ini() {
    local file="$1" section="" key="" value=""
    while IFS='=' read -r key value || [[ -n "$key" ]]; do
        key=$(echo "$key" | tr -d '[:space:]')
        value=$(echo "$value" | tr -d '[:space:]')
        [[ -z "$key" || "$key" == \#* || "$key" == ";"* ]] && continue
        if [[ "$key" == "["*"]" ]]; then
            section="${key:1:${#key}-2}"
            continue
        fi
        case "$section/$key" in
            general/name)      TV_NAME="$value" ;;
            general/count)     TV_COUNT="$value" ;;
            general/fs)        TV_FS="$value" ;;
            general/stripesize) TV_STRIPE="$value" ;;
            general/mount)     TV_MOUNT="$value" ;;
            general/total_gb)  TV_TOTAL="$value" ;;
            disk.*/*)          eval "TV_DISK_${section#disk.}_${key}=$value" ;;
        esac
    done < "$file"
}

restore_volume() {
    local conf_file="$1"
    local TV_NAME="" TV_COUNT="" TV_FS="ext4" TV_STRIPE="512" TV_MOUNT="" TV_TOTAL=""
    local -a DISK_DEVICE=() DISK_SIZE=()

    parse_ini "$conf_file"

    if [[ -z "$TV_NAME" ]]; then
        err "No volume name in $conf_file, skipping"
        return 1
    fi

    log "Restoring volume: $TV_NAME"

    # Collect disk info
    local i=0
    while [[ $i -lt ${TV_COUNT:-0} ]]; do
        local dev_var="TV_DISK_${i}_device"
        local sz_var="TV_DISK_${i}_size_gb"
        DISK_DEVICE+=("${!dev_var:-}")
        DISK_SIZE+=("${!sz_var:-0}")
        i=$((i + 1))
    done

    if [[ ${#DISK_DEVICE[@]} -lt 2 ]]; then
        err "Volume $TV_NAME needs at least 2 disks, found ${#DISK_DEVICE[@]}, skipping"
        return 1
    fi

    # Check all disks exist
    for dev in "${DISK_DEVICE[@]}"; do
        if [[ ! -b "/dev/$dev" ]]; then
            err "Disk /dev/$dev not found, skipping volume $TV_NAME"
            return 1
        fi
    done

    # Step 1: Clean up old targets
    log "  Step 1: Cleaning up old targets..."
    for dev in "${DISK_DEVICE[@]}"; do
        local target="tv_${dev}_carve"
        run sudo dmsetup remove "$target" 2>/dev/null || true
    done
    local vg_name="tv_vg_${TV_NAME}"
    local lv_name="tv_lv_${TV_NAME}"
    run sudo lvremove -f "${vg_name}/${lv_name}" 2>/dev/null || true
    run sudo vgremove -f "$vg_name" 2>/dev/null || true
    for dev in "${DISK_DEVICE[@]}"; do
        local target="tv_${dev}_carve"
        run sudo pvremove -ff -y "/dev/mapper/${target}" 2>/dev/null || true
    done

    # Step 2: Create dm-linear targets
    log "  Step 2: Creating dm-linear targets..."
    i=0
    while [[ $i -lt ${#DISK_DEVICE[@]} ]]; do
        local dev="${DISK_DEVICE[$i]}"
        local size_gb="${DISK_SIZE[$i]}"
        local target="tv_${dev}_carve"
        local sectors=$(( size_gb * 1024 * 1024 * 1024 / 512 ))

        local table="0 ${sectors} linear /dev/${dev} 0"
        if [[ $DRY_RUN -eq 1 ]]; then
            log "  DRY-RUN: dmsetup create $target (table: $table)"
        else
            echo "$table" | sudo dmsetup create "$target"
        fi
        if [[ $? -ne 0 ]]; then
            err "Failed to create dm target for /dev/$dev"
            return 1
        fi
        log "  Created $target (${size_gb}GB)"
        i=$((i + 1))
    done

    # Step 3: Create PV
    log "  Step 3: Creating LVM physical volumes..."
    for dev in "${DISK_DEVICE[@]}"; do
        local target="tv_${dev}_carve"
        run sudo pvcreate -f "/dev/mapper/${target}"
    done

    # Step 4: Create VG
    log "  Step 4: Creating volume group..."
    local pv_args=()
    for dev in "${DISK_DEVICE[@]}"; do
        pv_args+=("/dev/mapper/tv_${dev}_carve")
    done
    run sudo vgcreate "$vg_name" "${pv_args[@]}"

    # Step 5: Create striped LV
    log "  Step 5: Creating striped logical volume..."
    local stripes="${#DISK_DEVICE[@]}"
    run sudo lvcreate -l 100%FREE -i "$stripes" -I "${TV_STRIPE}k" -n "$lv_name" "$vg_name"

    local lv_path="/dev/mapper/${vg_name}-${lv_name}"

    # Step 6: Format
    if [[ "$TV_FS" != "none" ]]; then
        log "  Step 6: Formatting as $TV_FS..."
        run sudo "mkfs.${TV_FS}" "$lv_path"
    else
        log "  Step 6: Skipped formatting (raw)"
    fi

    # Step 7: Mount
    if [[ -n "$TV_MOUNT" && "$TV_FS" != "none" ]]; then
        log "  Step 7: Mounting at $TV_MOUNT..."
        run sudo mkdir -p "$TV_MOUNT"
        run sudo mount "$lv_path" "$TV_MOUNT"
    else
        log "  Step 7: Skipped mounting"
    fi

    log "  Volume $TV_NAME restored successfully!"
    return 0
}

# Main
if [[ ! -d "$CONF_DIR" ]]; then
    log "No config directory found at $CONF_DIR, nothing to restore"
    exit 0
fi

# Restore LVM volumes from .conf files
configs=("$CONF_DIR"/*.conf)
if [[ -e "${configs[0]}" ]]; then
    log "Found ${#configs[@]} LVM config(s) to restore"
    restored=0
    failed=0
    for conf in "${configs[@]}"; do
        if restore_volume "$conf"; then
            restored=$((restored + 1))
        else
            failed=$((failed + 1))
        fi
    done
    log "LVM restore complete: $restored succeeded, $failed failed"
fi

# Restore scheduler volumes from .scheduler files
scheduler_configs=("$CONF_DIR"/*.scheduler)
if [[ -e "${scheduler_configs[0]}" ]]; then
    log "Found ${#scheduler_configs[@]} scheduler config(s) to restore"
    for sched_file in "${scheduler_configs[@]}"; do
        local_name=$(basename "$sched_file" .scheduler)
        log "Restoring scheduler volume: $local_name"

        # Parse disk names from .scheduler metadata
        # Format: disk0_name=xxx, disk1_name=yyy
        disk_names=()
        while IFS='=' read -r key value; do
            key=$(echo "$key" | tr -d '[:space:]')
            value=$(echo "$value" | tr -d '[:space:]')
            [[ "$key" =~ ^disk[0-9]+_name$ ]] && disk_names+=("$value")
        done < "$sched_file"

        if [[ ${#disk_names[@]} -lt 2 ]]; then
            err "Scheduler volume $local_name needs at least 2 disks, found ${#disk_names[@]}, skipping"
            continue
        fi

        # Check all disks exist
        skip=0
        for dev in "${disk_names[@]}"; do
            if [[ ! -b "/dev/$dev" ]]; then
                err "Disk /dev/$dev not found, skipping volume $local_name"
                skip=1
                break
            fi
        done
        [[ $skip -eq 1 ]] && continue

        # Recreate dm-linear targets
        # We need the carve size. Parse from the .scheduler metadata or use
        # a reasonable default. The .scheduler file has segment info but not
        # the original carve size. We'll read the sector count from the
        # existing dm table if available, or skip.
        log "  Creating dm-linear targets for $local_name..."
        for dev in "${disk_names[@]}"; do
            target="tv_${dev}_carve"
            # Check if target already exists
            if run sudo dmsetup ls 2>/dev/null | grep -q "$target"; then
                log "  $target already exists, skipping"
                continue
            fi
            # Read from saved config if available
            saved_conf="$CONF_DIR/${local_name}.conf"
            size_gb=""
            if [[ -f "$saved_conf" ]]; then
                while IFS='=' read -r key value; do
                    key=$(echo "$key" | tr -d '[:space:]')
                    value=$(echo "$value" | tr -d '[:space:]')
                    if [[ "$key" == "device" && "$value" == "$dev" ]]; then
                        # Next line might be size_gb, need to read ahead
                        :
                    fi
                done < "$saved_conf"
            fi
            # Without carve size info, we cannot recreate the dm-linear target.
            # The user needs to re-create the scheduler volume with tiered_setup.
            err "  Cannot determine carve size for /dev/$dev from .scheduler file."
            err "  Please re-create the scheduler volume:"
            err "    sudo tiered_setup --create --name $local_name --disks $(IFS=,; echo "${disk_names[*]}") --scheduler"
        done
        log "  Scheduler volume $local_name restore complete (dm targets may need manual recreation)"
    done
fi

# === Restore kernel dm-target (tieredvol) volumes ===
# These configs have [weighted_striping] section with seg*_begin/end/ disks
restore_dm_target() {
    local conf_file="$1"
    local name="" disk_count=0 max_end=0
    local -a disk_names=()
    local in_section=0

    name=$(basename "$conf_file" .conf)

    # Check if already active
    if run sudo dmsetup ls 2>/dev/null | grep -q "^${name} "; then
        log "  $name already active, skipping"
        return 0
    fi

    # Parse [weighted_striping] section
    while IFS='=' read -r raw_key raw_value; do
        local key value
        key=$(echo "$raw_key" | tr -d '[:space:]')
        value=$(echo "$raw_value" | tr -d '[:space:]')
        [[ -z "$key" || "$key" == \#* || "$key" == ";"* ]] && continue

        if [[ "$key" == "[weighted_striping]" ]]; then
            in_section=1
            continue
        elif [[ "$key" == "["*"]" ]]; then
            in_section=0
            continue
        fi

        [[ $in_section -ne 1 ]] && continue

        case "$key" in
            disk_count)      disk_count="$value" ;;
            seg*_end)
                if [[ $value -gt $max_end ]]; then
                    max_end=$value
                fi
                ;;
        esac

        # Collect disk names (disk0_name=, disk1_name=, etc.)
        if [[ "$key" =~ ^disk[0-9]+_name$ ]]; then
            disk_names+=("$value")
        fi
    done < "$conf_file"

    if [[ ${#disk_names[@]} -eq 0 || $max_end -eq 0 ]]; then
        err "Cannot parse dm-target config $conf_file, skipping"
        return 1
    fi

    log "Restoring dm-target volume: $name (${#disk_names[@]} disks, $(( max_end / 512 )) sectors)"

    # Check all disks exist, try by-size fallback
    local fixed=0
    for i in "${!disk_names[@]}"; do
        local dev="${disk_names[$i]}"
        if [[ -b "$dev" ]]; then
            continue
        fi

        # Disk path not found — try to find by sector size
        local dev_name="${dev#/dev/}"
        local target_size
        target_size=$(blockdev --getsz "$dev" 2>/dev/null || echo "")

        if [[ -z "$target_size" ]]; then
            # Try lsblk to find a matching disk by name
            local alt=""
            alt=$(lsblk -dpno NAME,SIZE 2>/dev/null | awk -v orig="$dev_name" '
                BEGIN {}
                {
                    gsub(/[[:space:]]/, "", $0)
                    # Compare last 3 chars (sdb, sdc, etc.)
                }
            ')
        fi

        # Try /dev/disk/by-id or /dev/disk/by-path as last resort
        local found=""
        for by_id in /dev/disk/by-id/usb-* /dev/disk/by-id/ata-*; do
            if [[ -L "$by_id" ]] && [[ "$(readlink -f "$by_id")" == "$(readlink -f "/dev/$dev_name" 2>/dev/null)" ]]; then
                found="$by_id"
                break
            fi
        done

        if [[ -n "$found" && -b "$found" ]]; then
            log "  Mapping $dev -> $found (by-id)"
            disk_names[$i]="$found"
            fixed=1
        else
            err "Disk $dev not found, cannot restore $name"
            return 1
        fi
    done

    # If paths were fixed, update the config file
    if [[ $fixed -eq 1 ]]; then
        log "  Updating config with corrected disk paths..."
        local tmp_conf="${conf_file}.tmp"
        local idx=0
        while IFS= read -r line; do
            local key="${line%%=*}"
            key=$(echo "$key" | tr -d '[:space:]')
            if [[ "$key" =~ ^disk[0-9]+_name$ ]]; then
                echo "${key}=${disk_names[$idx]}"
                idx=$((idx + 1))
            else
                echo "$line"
            fi
        done < "$conf_file" > "$tmp_conf"
        run mv "$tmp_conf" "$conf_file"
    fi

    # Verify all disks are block devices
    for dev in "${disk_names[@]}"; do
        if [[ ! -b "$dev" ]]; then
            err "Disk $dev not found after fallback, cannot restore $name"
            return 1
        fi
    done

    # Compute total sectors from max_end (bytes -> sectors)
    local total_sectors=$(( max_end / 512 ))
    if [[ $total_sectors -le 0 ]]; then
        err "Invalid sector count for $name"
        return 1
    fi

    # Ensure tieredvol module is loaded
    if ! lsmod 2>/dev/null | grep -q "^tieredvol"; then
        log "  Loading tieredvol module..."
        run sudo modprobe tieredvol || {
            err "Failed to load tieredvol module"
            return 1
        }
    fi

    # Create the DM target volume
    local table="0 ${total_sectors} tieredvol ${conf_file}"
    log "  Creating DM target: $name ($total_sectors sectors)"
    if run sudo dmsetup create "$name" --table "$table"; then
        log "  Volume $name restored successfully!"
        return 0
    else
        err "Failed to create DM target $name"
        return 1
    fi
}

# Find and restore dm-target configs (files starting with 'tv_' that have [weighted_striping])
dm_target_restored=0
dm_target_failed=0
for conf in "$CONF_DIR"/*.conf; do
    [[ -e "$conf" ]] || continue

    # Detect [weighted_striping] section
    if grep -q '^\[weighted_striping\]' "$conf" 2>/dev/null; then
        if restore_dm_target "$conf"; then
            dm_target_restored=$((dm_target_restored + 1))
        else
            dm_target_failed=$((dm_target_failed + 1))
        fi
    fi
done

if [[ $dm_target_restored -gt 0 || $dm_target_failed -gt 0 ]]; then
    log "dm-target restore complete: $dm_target_restored succeeded, $dm_target_failed failed"
fi

exit 0
