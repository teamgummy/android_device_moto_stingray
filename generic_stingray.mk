$(call inherit-product, $(SRC_TARGET_DIR)/product/generic.mk)
$(call inherit-product, device/moto/stingray/device.mk)
$(call inherit-product-if-exists, vendor/moto/stingray/stingray-vendor.mk)

# Overrides
PRODUCT_DEVICE := stingray
PRODUCT_LOCALES += en_US
PRODUCT_MODEL := Motorola Stingray
PRODUCT_NAME := stingray