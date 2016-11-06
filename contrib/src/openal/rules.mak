OPENAL_VERSION := 1.17.2

OPENAL_URL := http://kcat.strangesoft.net/openal-releases/openal-soft-$(OPENAL_VERSION).tar.bz2

$(TARBALLS)/openal-soft-$(OPENAL_VERSION).tar.bz2:
	$(call download,$(OPENAL_URL))

ifeq ($(call need_pkg,"openal"),)
PKGS_FOUND += openal
endif
PKGS += openal


.sum-openal: openal-soft-$(OPENAL_VERSION).tar.bz2

libopenal: openal-soft-$(OPENAL_VERSION).tar.bz2 .sum-openal
	$(UNPACK)
	$(MOVE)

DEPS_libopenal = 

.openal: libopenal toolchain.cmake
	cd $< && $(HOSTVARS) $(CMAKE) \
		-DALSOFT_DLOPEN=OFF -DALSOFT_UTILS=OFF -DALSOFT_NO_CONFIG_UTIL=ON \
		-DALSOFT_EXAMPLES=OFF -DALSOFT_TESTS=OFF -DALSOFT_CONFIG=OFF \
		-DALSOFT_HRTF_SOFA=OFF -DALSOFT_AMBDEC_PRESETS=OFF -DALSOFT_HRTF_DEFS=OFF \
		-DALSOFT_INSTALL=OFF -DALSOFT_BACKEND_ALSA=OFF -DALSOFT_BACKEND_OSS=OFF \
		-DALSOFT_BACKEND_SOLARIS=OFF -DALSOFT_BACKEND_SNDIO=OFF -DALSOFT_BACKEND_QSA=OFF \
		-DALSOFT_BACKEND_WINMM=OFF -DALSOFT_BACKEND_DSOUND=OFF \
		-DALSOFT_BACKEND_MMDEVAP=OFF -DALSOFT_BACKEND_PORTAUDIO=OFF \
		-DALSOFT_BACKEND_PULSEAUDIO=OFF -DALSOFT_BACKEND_JACK=OFF \
		-DALSOFT_REQUIRE_COREAUDIO=OFF -DALSOFT_BACKEND_OPENSL=OFF \
		-DALSOFT_BACKEND_WAVE=OFF 
	cd $< && $(MAKE)
	mkdir -p -- "$(PREFIX)/include"
	cd $< && cp -vr include/AL "$(PREFIX)/include/"
	mkdir -p -- "$(PREFIX)/lib"
	cp -vf $</*.so "$(PREFIX)/lib/" || cp -vf $</*.dll "$(PREFIX)/lib/"
	touch $@	


