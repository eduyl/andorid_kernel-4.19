#!/usr/bin/env bash
cd /media/ian/SeagateHDD/android/google/common
ARCH=arm CROSS_COMPILE=/media/ian/CrucialP5_SSD/android/lineage-18-gts210ltexx/prebuilts/gcc/linux-x86/arm/arm-linux-androideabi-4.9/bin/arm-linux-androidkernel- make mrproper O="/media/ian/SeagateHDD/KernelOut"
ARCH=arm CROSS_COMPILE=/media/ian/CrucialP5_SSD/android/lineage-18-gts210ltexx/prebuilts/gcc/linux-x86/arm/arm-linux-androideabi-4.9/bin/arm-linux-androidkernel- make lineage_gts210ltexx_defconfig zImage dtbs modules -j4 O="/media/ian/SeagateHDD/KernelOut"
