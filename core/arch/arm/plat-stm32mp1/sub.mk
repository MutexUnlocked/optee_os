global-incdirs-y += .

srcs-y += main.c
srcs-y += reset.S
srcs-$(CFG_STM32_RNG) += rng_seed.c
srcs-y += scmi_server.c
srcs-y += shared_resources.c
srcs-$(CFG_TZC400) += plat_tzc400.c
srcs-$(CFG_RPROC_PTA) += remoteproc_pta.c

ifeq ($(CFG_RPROC_PTA),y)
gensrcs-y += rproc_pub_key
produce-rproc_pub_key = rproc_pub_key.c
depends-rproc_pub_key = $(CFG_RPROC_SIGN_KEY) scripts/pem_to_pub_c.py
recipe-rproc_pub_key = $(PYTHON3) scripts/pem_to_pub_c.py --prefix rproc_pub_key \
        --key $(CFG_RPROC_SIGN_KEY) --out $(sub-dir-out)/rproc_pub_key.c
endif

subdirs-y += drivers
subdirs-y += nsec-service
subdirs-y += pm
