# Offline producer, excluded from the serving image.
ifeq ($(MODELOS_REPOSITORY),)
$(error MODELOS_REPOSITORY must name the clean ModelOS main checkout)
endif
obj-y += capture.o
CFLAGS_capture.o += -DMODELOS_CAPTURE_SOURCE=\"$(MODELOS_REPOSITORY)/serve/hw/m3/capture.c\"
