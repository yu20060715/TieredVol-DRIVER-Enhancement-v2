#!/bin/bash
# Benchmark Runner v3 - Handles LVM PV corruption from TieredVol carving
# Strategy: Run all TieredVol benchmarks first, restore LVM PVs, then run LVM benchmarks
# Prerequisite: run 'sudo -v' first to cache credentials
set -e
WD=/home/yu/TieredVol
cd $WD

# Verify sudo is cached
if ! sudo -n true 2>/dev/null; then
    echo "Error: sudo credentials not cached. Run 'sudo -v' first."
    exit 1
fi
RUN_TS=$(date +%Y%m%d_%H%M%S)
mkdir -p $WD/benchmarks/run_${RUN_TS}
RDIR="benchmarks/run_${RUN_TS}"

echo "============================================="
echo "  TieredVol vs LVM Benchmark Suite v3"
echo "  Run: ${RUN_TS}"
echo "  Disk map: nvme0n1=NVMe(P3Plus), sdb=MX500, sdc=WDBlue"
echo "  Note: TieredVol carves overwrite LVM PV headers."
echo "        TieredVol benchmarks run first, then LVM."
echo "============================================="
echo ""

###############################################################################
# PHASE 1: ALL TIEREDVOL BENCHMARKS
###############################################################################

echo "============================================="
echo "  PHASE 1: TieredVol Benchmarks"
echo "============================================="

# T1: 2-disk 5GB Write
echo "[T1] TieredVol 2-disk 5GB write — 5 runs"
sudo ./tiered_setup --create --name tv_test_2d \
    --disks nvme0n1:100,sdb:100 --scheduler --yes 2>&1 | tee $RDIR/setup_tv_2d.txt

for i in 1 2 3 4 5; do
    echo "--- T1 Run $i ---" >> $RDIR/2disk_5gb_write_raw.txt
    sudo ./tiered_io --name tv_test_2d --bench --size 5GB 2>&1 | tee -a $RDIR/2disk_5gb_write_raw.txt
    sleep 10
done
sudo ./tiered_setup --destroy --name tv_test_2d 2>&1 || true
sleep 10

# T2: 2-disk 5GB Read
echo "[T2] TieredVol 2-disk 5GB read — 5 runs"
sudo ./tiered_setup --create --name tv_test_2d \
    --disks nvme0n1:100,sdb:100 --scheduler --yes 2>&1 | tee $RDIR/setup_tv_2d_read.txt

for i in 1 2 3 4 5; do
    echo "--- T2 Run $i ---" >> $RDIR/2disk_5gb_read_raw.txt
    sudo ./tiered_io --name tv_test_2d --bench-read --size 5GB 2>&1 | tee -a $RDIR/2disk_5gb_read_raw.txt
    sleep 10
done
sudo ./tiered_setup --destroy --name tv_test_2d 2>&1 || true
sleep 10

# T3: 2-disk 512MB Write
echo "[T3] TieredVol 2-disk 512MB write — 5 runs"
sudo ./tiered_setup --create --name tv_test_2d \
    --disks nvme0n1:100,sdb:100 --scheduler --yes 2>&1 | tee $RDIR/setup_tv_2d_512.txt

for i in 1 2 3 4 5; do
    echo "--- T3 Run $i ---" >> $RDIR/2disk_512mb_write_raw.txt
    sudo ./tiered_io --name tv_test_2d --bench --size 512MB 2>&1 | tee -a $RDIR/2disk_512mb_write_raw.txt
    sleep 10
done
sudo ./tiered_setup --destroy --name tv_test_2d 2>&1 || true
sleep 10

# T4: 3-disk 5GB Write
echo "[T4] TieredVol 3-disk 5GB write — 5 runs"
sudo ./tiered_setup --create --name tv_test_3d \
    --disks nvme0n1:100,sdb:100,sdc:100 --scheduler --yes 2>&1 | tee $RDIR/setup_tv_3d.txt

for i in 1 2 3 4 5; do
    echo "--- T4 Run $i ---" >> $RDIR/3disk_5gb_write_raw.txt
    sudo ./tiered_io --name tv_test_3d --bench --size 5GB 2>&1 | tee -a $RDIR/3disk_5gb_write_raw.txt
    sleep 10
done
sudo ./tiered_setup --destroy --name tv_test_3d 2>&1 || true
sleep 10

# T5: 3-disk 5GB Read
echo "[T5] TieredVol 3-disk 5GB read — 5 runs"
sudo ./tiered_setup --create --name tv_test_3d \
    --disks nvme0n1:100,sdb:100,sdc:100 --scheduler --yes 2>&1 | tee $RDIR/setup_tv_3d_read.txt

for i in 1 2 3 4 5; do
    echo "--- T5 Run $i ---" >> $RDIR/3disk_5gb_read_raw.txt
    sudo ./tiered_io --name tv_test_3d --bench-read --size 5GB 2>&1 | tee -a $RDIR/3disk_5gb_read_raw.txt
    sleep 10
done
sudo ./tiered_setup --destroy --name tv_test_3d 2>&1 || true
sleep 10

# T6: Chunk Size Sweep (256KB, 512KB)
echo "[T6] TieredVol chunk size sweep — 3 runs each"
for CHUNK_KB in 256 512; do
    echo "[T6] Rebuilding with TV_CHUNK_SIZE=${CHUNK_KB}KB"
    sed -i "s/#define TV_CHUNK_SIZE.*/#define TV_CHUNK_SIZE (${CHUNK_KB} * 1024)/" src/tieredvol_types.h
    make clean && make 2>&1 | tail -3

    sudo ./tiered_setup --create --name tv_chunk_${CHUNK_KB} \
        --disks nvme0n1:100,sdb:100 --scheduler --yes 2>&1

    for i in 1 2 3; do
        echo "--- Run $i (chunk=${CHUNK_KB}KB) ---" >> $RDIR/chunksize_${CHUNK_KB}kb_raw.txt
        sudo ./tiered_io --name tv_chunk_${CHUNK_KB} --bench --size 5GB 2>&1 | tee -a $RDIR/chunksize_${CHUNK_KB}kb_raw.txt
        sleep 10
    done
    sudo ./tiered_setup --destroy --name tv_chunk_${CHUNK_KB} 2>&1 || true
    sleep 10
done

# Restore default chunk size
echo "Restoring default chunk size (1MB)..."
sed -i "s/#define TV_CHUNK_SIZE.*/#define TV_CHUNK_SIZE (1 * 1024 * 1024)/" src/tieredvol_types.h
make clean && make 2>&1 | tail -3
sleep 5

# T7: io_uring metrics
echo "[T7] io_uring metrics — strace + perf stat"
sudo ./tiered_setup --create --name tv_test_3d \
    --disks nvme0n1:100,sdb:100,sdc:100 --scheduler --yes 2>&1

echo "[T7.1] strace io_uring_enter"
sudo strace -e trace=io_uring_enter -c \
    ./tiered_io --name tv_test_3d --bench --size 5GB 2>&1 | tee $RDIR/uring_3disk_strace_count.txt
sleep 10

echo "[T7.2] perf stat"
sudo perf stat -e syscalls:sys_enter_io_uring_enter,syscalls:sys_exit_io_uring_enter \
    ./tiered_io --name tv_test_3d --bench --size 5GB 2>&1 | tee $RDIR/uring_3disk_perf_stat.txt

sudo ./tiered_setup --destroy --name tv_test_3d 2>&1 || true
sleep 10

###############################################################################
# PHASE 2: RESTORE LVM PV HEADERS
###############################################################################
echo ""
echo "============================================="
echo "  PHASE 2: Restoring LVM PV headers"
echo "============================================="

# Remove any leftover PV headers from previous LVM setup
sudo vgs tv_lvm_vg 2>/dev/null | grep -q tv_lvm_vg && \
    sudo vgreduce --removemissing tv_lvm_vg --force 2>/dev/null || true

# Re-create PVs on nvme0n1 and sdb (overwritten by TieredVol carving)
sudo pvcreate -f /dev/nvme0n1 2>&1
sudo pvcreate -f /dev/sdb 2>&1

# Extend VG
sudo vgextend tv_lvm_vg /dev/nvme0n1 /dev/sdb 2>&1

echo "LVM state after restore:"
sudo vgs 2>/dev/null
sudo pvs 2>/dev/null

# Verify LVM works
sudo lvcreate -L 1G -n lvm_verify tv_lvm_vg 2>&1
LVV=$(sudo lvs --noheadings -o lv_path tv_lvm_vg/lvm_verify 2>&1 | tr -d ' ')
echo "Verification LV: $LVV"
sudo ./tiered_io --path "$LVV" --bench --size 64MB --raw 2>&1 | grep "Throughput"
sudo lvremove -f tv_lvm_vg/lvm_verify 2>&1
echo "LVM verification complete."

###############################################################################
# PHASE 3: ALL LVM BENCHMARKS
###############################################################################
echo ""
echo "============================================="
echo "  PHASE 3: LVM Striped Benchmarks"
echo "============================================="

# L1: LVM 2-disk NVMe+SATA 5GB Write
echo "[L1] LVM 2-disk NVMe+SATA 5GB write — 5 runs"
sudo lvcreate -L 100G -i 2 -I 256k -n lvm_bench_2d tv_lvm_vg /dev/nvme0n1 /dev/sdb 2>&1
LV2D=$(sudo lvs --noheadings -o lv_path tv_lvm_vg/lvm_bench_2d 2>&1 | tr -d ' ')
echo "LV path: $LV2D"

for i in 1 2 3 4 5; do
    echo "--- L1 Run $i ---" >> $RDIR/lvm_nvsata_2disk_5gb_write_raw.txt
    sudo ./tiered_io --path "$LV2D" --bench --size 5GB --raw 2>&1 | tee -a $RDIR/lvm_nvsata_2disk_5gb_write_raw.txt
    sleep 10
done
sudo lvremove -f tv_lvm_vg/lvm_bench_2d 2>&1
sleep 10

# L2: LVM 2-disk NVMe+SATA 5GB Read
echo "[L2] LVM 2-disk NVMe+SATA 5GB read — 5 runs"
sudo lvcreate -L 100G -i 2 -I 256k -n lvm_bench_2d tv_lvm_vg /dev/nvme0n1 /dev/sdb 2>&1
LV2D=$(sudo lvs --noheadings -o lv_path tv_lvm_vg/lvm_bench_2d 2>&1 | tr -d ' ')

for i in 1 2 3 4 5; do
    echo "--- L2 Run $i ---" >> $RDIR/lvm_nvsata_2disk_5gb_read_raw.txt
    sudo ./tiered_io --path "$LV2D" --bench-read --size 5GB --raw 2>&1 | tee -a $RDIR/lvm_nvsata_2disk_5gb_read_raw.txt
    sleep 10
done
sudo lvremove -f tv_lvm_vg/lvm_bench_2d 2>&1
sleep 10

# L3: LVM 2-disk NVMe+SATA 512MB Write
echo "[L3] LVM 2-disk NVMe+SATA 512MB write — 5 runs"
sudo lvcreate -L 100G -i 2 -I 256k -n lvm_bench_2d tv_lvm_vg /dev/nvme0n1 /dev/sdb 2>&1
LV2D=$(sudo lvs --noheadings -o lv_path tv_lvm_vg/lvm_bench_2d 2>&1 | tr -d ' ')

for i in 1 2 3 4 5; do
    echo "--- L3 Run $i ---" >> $RDIR/lvm_nvsata_2disk_512mb_write_raw.txt
    sudo ./tiered_io --path "$LV2D" --bench --size 512MB --raw 2>&1 | tee -a $RDIR/lvm_nvsata_2disk_512mb_write_raw.txt
    sleep 10
done
sudo lvremove -f tv_lvm_vg/lvm_bench_2d 2>&1
sleep 10

# L4: LVM 3-disk NVMe+2×SATA 5GB Write
echo "[L4] LVM 3-disk NVMe+2×SATA 5GB write — 5 runs"
sudo lvcreate -L 100G -i 3 -I 256k -n lvm_bench_3d tv_lvm_vg 2>&1
LV3D=$(sudo lvs --noheadings -o lv_path tv_lvm_vg/lvm_bench_3d 2>&1 | tr -d ' ')
echo "LV path: $LV3D"

for i in 1 2 3 4 5; do
    echo "--- L4 Run $i ---" >> $RDIR/lvm_nvsata_3disk_5gb_write_raw.txt
    sudo ./tiered_io --path "$LV3D" --bench --size 5GB --raw 2>&1 | tee -a $RDIR/lvm_nvsata_3disk_5gb_write_raw.txt
    sleep 10
done
sudo lvremove -f tv_lvm_vg/lvm_bench_3d 2>&1
sleep 10

# L5: LVM 3-disk NVMe+2×SATA 5GB Read
echo "[L5] LVM 3-disk NVMe+2×SATA 5GB read — 5 runs"
sudo lvcreate -L 100G -i 3 -I 256k -n lvm_bench_3d tv_lvm_vg 2>&1
LV3D=$(sudo lvs --noheadings -o lv_path tv_lvm_vg/lvm_bench_3d 2>&1 | tr -d ' ')

for i in 1 2 3 4 5; do
    echo "--- L5 Run $i ---" >> $RDIR/lvm_nvsata_3disk_5gb_read_raw.txt
    sudo ./tiered_io --path "$LV3D" --bench-read --size 5GB --raw 2>&1 | tee -a $RDIR/lvm_nvsata_3disk_5gb_read_raw.txt
    sleep 10
done
sudo lvremove -f tv_lvm_vg/lvm_bench_3d 2>&1
sleep 10

# L6: LVM Stripe Size Sweep
echo "[L6] LVM stripe size sweep — 3 runs each"
for STRIPE in 128 256 512 1024; do
    echo "[L6] LVM stripe=${STRIPE}KB 5GB write — 3 runs"
    sudo lvcreate -L 100G -i 2 -I ${STRIPE}k -n lvm_stripe_${STRIPE} tv_lvm_vg /dev/nvme0n1 /dev/sdb 2>&1
    LVPATH=$(sudo lvs --noheadings -o lv_path tv_lvm_vg/lvm_stripe_${STRIPE} 2>&1 | tr -d ' ')
    echo "LV path: $LVPATH"

    for i in 1 2 3; do
        echo "--- Run $i (stripe=${STRIPE}KB) ---" >> $RDIR/lvm_stripesize_${STRIPE}kb_raw.txt
        sudo ./tiered_io --path "$LVPATH" --bench --size 5GB --raw 2>&1 | tee -a $RDIR/lvm_stripesize_${STRIPE}kb_raw.txt
        sleep 10
    done
    sudo lvremove -f tv_lvm_vg/lvm_stripe_${STRIPE} 2>&1
    sleep 10
done

###############################################################################
# FINAL: Cleanup
###############################################################################
echo ""
echo "============================================="
echo "  CLEANUP"
echo "============================================="
# Destroy any remaining TV volumes
for vol in $(sudo ./tiered_setup --list 2>/dev/null | grep "tv_\|chunk" | awk '{print $1}' 2>/dev/null); do
    sudo ./tiered_setup --destroy --name "$vol" 2>/dev/null || true
done

echo ""
echo "============================================="
echo "  ALL BENCHMARKS COMPLETE"
echo "============================================="
echo "Run timestamp: ${RUN_TS}"
echo "Raw output in: $RDIR"
ls -la $RDIR/*.txt
