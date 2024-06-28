#!/usr/bin/env bash
cd /media/jan/android/kernel-4.19
ARCH=arm CROSS_COMPILE=/media/jan/android/LOS-UL/prebuilts/gcc/linux-x86/arm/arm-linux-androideabi-4.9/bin/arm-linux-androidkernel- make mrproper O="/media/jan/android/KernelOut"
ARCH=arm CROSS_COMPILE=/media/jan/android/LOS-UL/prebuilts/gcc/linux-x86/arm/arm-linux-androideabi-4.9/bin/arm-linux-androidkernel- make lineage_gts210ltexx_defconfig zImage dtbs modules -j4 O="/media/jan/android/KernelOut"
