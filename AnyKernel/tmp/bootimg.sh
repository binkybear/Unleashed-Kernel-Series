#!/sbin/sh
mkdir /tmp/out
if [ -e /tmp/boot.img ]; then
	/tmp/unpackbootimg -i /tmp/boot.img -o /tmp/out
else
	echo "boot.img dump failed!" | tee /dev/kmsg
	exit 1
fi
rm -rf /tmp/boot.img
if [ -e /tmp/out/boot.img-ramdisk.gz ]; then
	rdcomp=/tmp/out/boot.img-ramdisk.gz
	echo "[AnyKernel] New ramdisk uses GZ compression." | tee /dev/kmsg
elif [ -e /tmp/out/boot.img-ramdisk.lz4 ]; then
	rdcomp=/tmp/out/boot.img-ramdisk.lz4
	echo "[AnyKernel] New ramdisk uses LZ4 compression." | tee /dev/kmsg
else
	echo "[AnyKernel] Unknown ramdisk format!" | tee /dev/kmsg
	exit 1
fi
/tmp/mkbootimg --kernel /tmp/kernel --ramdisk $rdcomp --cmdline "$(cat /tmp/out/boot.img-cmdline)" --base $(cat /tmp/out/boot.img-base) --pagesize $(cat /tmp/out/boot.img-pagesize) --ramdisk_offset $(cat /tmp/out/boot.img-ramdisk_offset) --tags_offset $(cat /tmp/out/boot.img-tags_offset) -o /tmp/boot.img
if [ -e /tmp/boot.img ]; then
	echo "[AnyKernel] Boot.img created successfully!" | tee /dev/kmsg
else
	echo "[AnyKernel] Boot.img failed to create!" | tee /dev/kmsg
	exit 1
fi
