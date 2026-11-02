cmake_minimum_required(VERSION 3.13)

project(darling-dmg CXX)

set(dmg_sources
	src/unichar.cpp
	src/Reader.cpp
	src/FileReader.cpp
	src/HFSVolume.cpp
	src/AppleDisk.cpp
	src/SubReader.cpp
	src/HFSBTree.cpp
	src/HFSFork.cpp
	src/HFSCatalogBTree.cpp
	src/HFSExtentsOverflowBTree.cpp
	src/HFSAttributeBTree.cpp

	src/DMGDisk.cpp
	src/DMGPartition.cpp
	src/DMGDecompressor.cpp
	src/adc.cpp
	src/HFSZlibReader.cpp
	src/MemoryReader.cpp

	src/GPTDisk.cpp
	
	src/MacBinary.cpp
	src/ResourceFork.cpp
	src/CacheZone.cpp
	src/CachedReader.cpp

	src/HFSHighLevelVolume.cpp

	src/APFSBTreeNode.cpp
	src/APFSContainer.cpp
	src/APFSVolume.cpp
)


# This part of CMakeLists.txt is for when building darling-dmg
# as a standalone FUSE module outside of Darling.
if (NOT DARLING)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH}
	"${CMAKE_CURRENT_SOURCE_DIR}/cmake_modules/")

add_definitions(-D_FILE_OFFSET_BITS=64)
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ggdb -O0")

include(FindLibXml2)

find_package(LibXml2 REQUIRED)
include_directories(${LIBXML2_INCLUDE_DIR})

# LZFSE chunks, common in APFS images, need Apple's open-source lzfse library
# (https://github.com/lzfse/lzfse), which few distributions package. Add its install
# prefix to CMAKE_PREFIX_PATH to enable them; without it they fail to decompress.
find_path(LZFSE_INCLUDE_DIR lzfse.h)
find_library(LZFSE_LIBRARY lzfse)
set(DMG_LZFSE_LIBRARIES "")
if (LZFSE_INCLUDE_DIR AND LZFSE_LIBRARY)
	add_definitions(-DCOMPILE_WITH_LZFSE=1)
	include_directories(${LZFSE_INCLUDE_DIR})
	set(DMG_LZFSE_LIBRARIES ${LZFSE_LIBRARY})
endif ()

if (WITH_TESTS)
	enable_testing()
	find_package(Boost COMPONENTS unit_test_framework REQUIRED)

	add_definitions(-DBOOST_TEST_DYN_LINK ) # -DDEBUG

	set(CacheTest_SRC
		test/CacheTest.cpp
		src/CacheZone.cpp
		src/CachedReader.cpp
		src/Reader.cpp
		src/MemoryReader.cpp
	)

	add_executable(CacheTest ${CacheTest_SRC})
	target_link_libraries(CacheTest ${Boost_UNIT_TEST_FRAMEWORK_LIBRARY})
	add_test(NAME CacheTest COMMAND CacheTest)
endif (WITH_TESTS)

add_library(dmg SHARED
	src/unichar.cpp
	src/Reader.cpp
	src/FileReader.cpp
	src/HFSVolume.cpp
	src/AppleDisk.cpp
	src/SubReader.cpp
	src/HFSBTree.cpp
	src/HFSFork.cpp
	src/HFSCatalogBTree.cpp
	src/HFSExtentsOverflowBTree.cpp
	src/HFSAttributeBTree.cpp

	src/DMGDisk.cpp
	src/DMGPartition.cpp
	src/DMGDecompressor.cpp
	src/adc.cpp
	src/HFSZlibReader.cpp
	src/MemoryReader.cpp

	src/GPTDisk.cpp
	
	src/MacBinary.cpp
	src/ResourceFork.cpp
	src/CacheZone.cpp
	src/CachedReader.cpp

	src/HFSHighLevelVolume.cpp

	src/APFSBTreeNode.cpp
	src/APFSContainer.cpp
	src/APFSVolume.cpp
)
target_compile_features(dmg PRIVATE cxx_std_11)
target_link_libraries(dmg -licuuc -lcrypto -lz -lbz2 -llzma ${DMG_LZFSE_LIBRARIES} ${LIBXML2_LIBRARY})
install(TARGETS dmg DESTINATION lib)

add_executable(darling-dmg
	src/main-fuse.cpp
)

SET(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")
SET(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE) 
SET(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

target_link_libraries(darling-dmg -lfuse dmg)
install(TARGETS darling-dmg RUNTIME DESTINATION bin)

add_executable(darling-dmg-extract src/main-extract.cpp src/DMGExtractor.cpp)
target_compile_features(darling-dmg-extract PRIVATE cxx_std_11)
target_link_libraries(darling-dmg-extract dmg)
install(TARGETS darling-dmg-extract RUNTIME DESTINATION bin)

# Vendor disk images are too large to commit. Point DMG_TEST_ARTIFACTS, DMG_TEST_INPUTS or both
# at directories laid out as <app>/<version>/{<image>,bundle-content-manifest.json} to compare
# darling-dmg-extract's output with manifests recorded from an independent extractor (7-Zip).
set(DMG_TEST_ARTIFACTS "" CACHE PATH "Directory of vendor disk images and their manifests")
set(DMG_TEST_INPUTS "" CACHE PATH "Directory of regression disk images and their manifests")
if (DMG_TEST_ARTIFACTS OR DMG_TEST_INPUTS)
	if (NOT DMG_LZFSE_LIBRARIES)
		message(FATAL_ERROR "The image tests need lzfse, because the CotEditor image is "
			"stored in LZFSE chunks; add an lzfse install prefix to CMAKE_PREFIX_PATH")
	endif ()
	enable_testing()
	find_package(Python3 COMPONENTS Interpreter REQUIRED)

	function(add_extract_test name root image bundle)
		get_filename_component(dir "${root}/${image}" DIRECTORY)
		add_test(NAME ${name}
			COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/check-extract.py
				$<TARGET_FILE:darling-dmg-extract> ${root}/${image}
				${dir}/bundle-content-manifest.json ${bundle})
		set_tests_properties(${name} PROPERTIES ENVIRONMENT "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:dmg>")
	endfunction()
endif ()

if (DMG_TEST_ARTIFACTS)
	# HFSX volume stored in LZMA chunks
	add_extract_test(extract-firefox-159 ${DMG_TEST_ARTIFACTS} firefox/159.0/Firefox-159.0.dmg Firefox.app)
	# APFS volume in a GPT partition, stored in LZFSE chunks
	add_extract_test(extract-coteditor-7.1.3 ${DMG_TEST_ARTIFACTS} coteditor/7.1.3/CotEditor_7.1.3.dmg CotEditor.app)
endif (DMG_TEST_ARTIFACTS)

if (DMG_TEST_INPUTS)
	# Partition names with fullwidth punctuation, read through the partition-map fallback
	add_extract_test(extract-clipy-1.4.1 ${DMG_TEST_INPUTS} clipy/1.4.1/Clipy_1.4.1.dmg Clipy.app)
	add_extract_test(extract-excalidrawz-2.5.1 ${DMG_TEST_INPUTS} excalidrawz/2.5.1/ExcalidrawZ.2.5.1.dmg ExcalidrawZ.app)
	add_extract_test(extract-kubeterm-2.8.3 ${DMG_TEST_INPUTS} kubeterm/2.8.3/Kubeterm-v2.8.3-universal.dmg kubeterm.app)
	add_extract_test(extract-mweb-pro-4.8.4 ${DMG_TEST_INPUTS} mweb-pro/4.8.4/MWebPro484.dmg "MWeb Pro.app")
	# HFS+ partition named only "disk image"
	add_extract_test(extract-dcommander-3.9.9.2 ${DMG_TEST_INPUTS} dcommander/3.9.9.2/DCommander-3992.dmg DCommander.app)
	# A single blkx holding the whole disk
	add_extract_test(extract-sonic3air-26.03.28 ${DMG_TEST_INPUTS} sonic3air/26.03.28.0/sonic3air_mac_260328.dmg "Sonic 3 AIR.app")
endif (DMG_TEST_INPUTS)

# This is used in Darling build.
else (NOT DARLING)
	include(darling_exe)
	include(FindPkgConfig)

	add_definitions(-D_FILE_OFFSET_BITS=64 -DCOMPILE_WITH_LZFSE=1)

	option(DARLING_DMG_ENABLE_FUSE "Build guest hdiutil with FUSE mounting support" OFF)
	if (DARLING_DMG_ENABLE_FUSE)
		include(wrap_elf)
		pkg_check_modules(FUSE REQUIRED fuse)
		include_directories(${FUSE_INCLUDE_DIRS})
		wrap_elf(fuse libfuse.so)
	endif()

	SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -nostdinc")

	add_darling_executable(hdiutil
		${dmg_sources}
		src/DMGExtractor.cpp
		src/main-hdiutil.cpp
	)

	if (DARLING_DMG_ENABLE_FUSE)
		target_link_libraries(hdiutil fuse)
	else()
		target_compile_definitions(hdiutil PRIVATE DARLING_DMG_NO_FUSE)
	endif()
	target_link_libraries(hdiutil icucore z bz2 crypto44 xml2 iconv lzfse lzma)
	install(TARGETS hdiutil DESTINATION libexec/darling/usr/bin)

endif (NOT DARLING)
