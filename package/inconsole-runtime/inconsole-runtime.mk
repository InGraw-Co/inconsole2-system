################################################################################
#
# inconsole-runtime
#
################################################################################

INCONSOLE_RUNTIME_VERSION = 2.4
INCONSOLE_RUNTIME_SITE = $(INCONSOLE_RUNTIME_PKGDIR)/src
INCONSOLE_RUNTIME_SITE_METHOD = local
INCONSOLE_RUNTIME_LICENSE = Proprietary
INCONSOLE_RUNTIME_DEPENDENCIES = sdl freetype libpng dejavu

INCONSOLE_RUNTIME_SRCS = \
	main.cpp \
	runtime_core.cpp \
	runtime_input.cpp \
	runtime_renderer.cpp \
	runtime_scenes.cpp

define INCONSOLE_RUNTIME_BUILD_CMDS
	$(TARGET_CXX) $(TARGET_CXXFLAGS) -std=c++17 -O2 -Wall -Wextra \
		-I$(STAGING_DIR)/usr/include/SDL \
		-I$(STAGING_DIR)/usr/include/freetype2 \
		-I$(STAGING_DIR)/usr/include/libpng16 \
		$(TARGET_LDFLAGS) $(addprefix $(@D)/,$(INCONSOLE_RUNTIME_SRCS)) \
		-L$(STAGING_DIR)/usr/lib -lSDL -lfreetype -lpng -lz -lm -lpthread \
		-o $(@D)/inconsole-runtime
	$(TARGET_CXX) $(TARGET_CXXFLAGS) -std=c++17 -O2 -Wall -Wextra \
		$(TARGET_LDFLAGS) $(@D)/input_bridge.cpp \
		-o $(@D)/inconsole-input-bridge
endef

define INCONSOLE_RUNTIME_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/inconsole-runtime \
		$(TARGET_DIR)/usr/bin/inconsole-runtime
	$(INSTALL) -D -m 0755 $(@D)/inconsole-input-bridge \
		$(TARGET_DIR)/usr/bin/inconsole-input-bridge
	$(INSTALL) -d $(TARGET_DIR)/usr/share/inconsole/system-icons
	cp -a $(INCONSOLE_RUNTIME_PKGDIR)/assets/system-icons/. \
		$(TARGET_DIR)/usr/share/inconsole/system-icons/
	$(INSTALL) -d $(TARGET_DIR)/usr/share/inconsole/default-apps
	cp -a $(INCONSOLE_RUNTIME_PKGDIR)/userdata/apps/. \
		$(TARGET_DIR)/usr/share/inconsole/default-apps/
	if [ -f $(TARGET_DIR)/usr/share/inconsole/default-apps/doom/launch.sh ]; then chmod +x $(TARGET_DIR)/usr/share/inconsole/default-apps/doom/launch.sh; fi

	rm -rf $(TARGET_DIR)/userdata
	$(INSTALL) -d $(TARGET_DIR)/userdata
	cp -a $(INCONSOLE_RUNTIME_PKGDIR)/userdata/. \
		$(TARGET_DIR)/userdata/
	rm -f $(TARGET_DIR)/userdata/system/profile.json
	chmod +x $(TARGET_DIR)/userdata/system/postscript.sh
	chmod +x $(TARGET_DIR)/userdata/system/undo_postscript.sh
	if [ -f $(TARGET_DIR)/userdata/apps/doom/launch.sh ]; then chmod +x $(TARGET_DIR)/userdata/apps/doom/launch.sh; fi
endef

$(eval $(generic-package))
