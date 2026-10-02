RACK_DIR ?= ../Rack-SDK

SLUG := $(shell jq -r .slug plugin.json)
VERSION := $(shell jq -r .version plugin.json)

SOURCES += src/plugin.cpp
SOURCES += src/Balatube.cpp
# Vocal Lamma NO se compila aqui: sigue publicandose como el plugin propio
# "VocalLamma" en la Library. Incluirlo duplicaria el slug de modulo (Rack
# resolveria los patches al plugin equivocado). El fuente queda intacto en
# src/VocalLamma.cpp por si vuelve a este plugin.

DISTRIBUTABLES += res

include $(RACK_DIR)/plugin.mk