# ogle-extract: the level extractor (see main.cpp). Built from the data readers of OpenGOAL's
# decompiler, vendored in opengoal/ with jak-project's paths (ISC license, opengoal/LICENSE):
# only the files the extraction needs are compiled and kept.

set(OG "${CMAKE_CURRENT_SOURCE_DIR}/opengoal")

add_library(ogle_extract_fmt STATIC "${OG}/third-party/fmt/src/format.cc" "${OG}/third-party/fmt/src/os.cc")
target_include_directories(ogle_extract_fmt PUBLIC "${OG}/third-party/fmt/include")

set(OGLE_EXTRACT_SOURCES
  main.cpp

  ${OG}/common/custom_data/pack_helpers.cpp
  ${OG}/common/custom_data/TFrag3Data.cpp
  ${OG}/common/dma/dma.cpp
  ${OG}/common/dma/gs.cpp
  ${OG}/common/goos/Object.cpp
  ${OG}/common/goos/ParseHelpers.cpp
  ${OG}/common/goos/PrettyPrinter.cpp
  ${OG}/common/goos/PrettyPrinter2.cpp
  ${OG}/common/goos/Printer.cpp
  ${OG}/common/goos/Reader.cpp
  ${OG}/common/goos/TextDB.cpp
  ${OG}/common/log/log.cpp
  ${OG}/common/math/geometry.cpp
  ${OG}/common/texture/texture_slots.cpp
  ${OG}/common/type_system/defenum.cpp
  ${OG}/common/type_system/deftype.cpp
  ${OG}/common/type_system/state.cpp
  ${OG}/common/type_system/Type.cpp
  ${OG}/common/type_system/TypeFieldLookup.cpp
  ${OG}/common/type_system/TypeSpec.cpp
  ${OG}/common/type_system/TypeSystem.cpp
  ${OG}/common/util/Assert.cpp
  ${OG}/common/util/BitUtils.cpp
  ${OG}/common/util/crc32.cpp
  ${OG}/common/util/dgo_util.cpp
  ${OG}/common/util/diff.cpp
  ${OG}/common/util/DgoReader.cpp
  ${OG}/common/util/FileUtil.cpp
  ${OG}/common/util/image_resize.cpp
  ${OG}/common/util/gltf_util.cpp
  ${OG}/common/util/json_util.cpp
  ${OG}/common/util/os.cpp
  ${OG}/common/util/print_float.cpp
  ${OG}/common/util/read_iso_file.cpp
  ${OG}/common/util/SimpleThreadGroup.cpp
  ${OG}/common/util/string_util.cpp
  ${OG}/common/util/Timer.cpp
  ${OG}/common/util/unicode_util.cpp
  ${OG}/common/versions/versions.cpp

  ${OG}/decompiler/data/dir_tpages.cpp
  ${OG}/decompiler/data/StrFileReader.cpp
  ${OG}/decompiler/data/TextureDB.cpp
  ${OG}/decompiler/data/tpage.cpp
  ${OG}/decompiler/Disasm/Instruction.cpp
  ${OG}/decompiler/Disasm/InstructionDecode.cpp
  ${OG}/decompiler/Disasm/InstructionMatching.cpp
  ${OG}/decompiler/Disasm/OpcodeInfo.cpp
  ${OG}/decompiler/Disasm/Register.cpp
  ${OG}/decompiler/extractor/extractor_util.cpp
  ${OG}/decompiler/Function/BasicBlocks.cpp
  ${OG}/decompiler/Function/CfgVtx.cpp
  ${OG}/decompiler/Function/Function.cpp
  ${OG}/decompiler/level_extractor/BspHeader.cpp
  ${OG}/decompiler/level_extractor/extract_actors.cpp
  ${OG}/decompiler/level_extractor/extract_anim.cpp
  ${OG}/decompiler/level_extractor/extract_collide_frags.cpp
  ${OG}/decompiler/level_extractor/extract_common.cpp
  ${OG}/decompiler/level_extractor/extract_hfrag.cpp
  ${OG}/decompiler/level_extractor/extract_joint_group.cpp
  ${OG}/decompiler/level_extractor/extract_level.cpp
  ${OG}/decompiler/level_extractor/extract_merc.cpp
  ${OG}/decompiler/level_extractor/extract_shrub.cpp
  ${OG}/decompiler/level_extractor/extract_tfrag.cpp
  ${OG}/decompiler/level_extractor/extract_tie.cpp
  ${OG}/decompiler/level_extractor/fr3_to_gltf.cpp
  ${OG}/decompiler/level_extractor/merc_replacement.cpp
  ${OG}/decompiler/level_extractor/MercData.cpp
  ${OG}/decompiler/level_extractor/tfrag_tie_fixup.cpp
  ${OG}/decompiler/ObjectFile/LinkedObjectFile.cpp
  ${OG}/decompiler/ObjectFile/LinkedObjectFileCreation.cpp
  ${OG}/decompiler/ObjectFile/ObjectFileDB.cpp
  ${OG}/decompiler/util/DecompilerTypeSystem.cpp
  ${OG}/decompiler/util/goal_data_reader.cpp
  ${OG}/decompiler/util/TP_Type.cpp

  ${OG}/third-party/lzokay/lzokay.cpp
  ${OG}/third-party/zstd/lib/common/xxhash.c
  stubs.cpp
  ${OG}/third-party/stb_image/stb_image.cpp
  ${OG}/third-party/tiny_gltf/tiny_gltf.cpp
)

add_executable(ogle-extract ${OGLE_EXTRACT_SOURCES})
target_include_directories(ogle-extract PRIVATE "${OG}" "${OG}/third-party" "${OG}/third-party/stb_image")
target_link_libraries(ogle-extract PRIVATE ogle_extract_fmt)
if(WIN32)
  target_compile_definitions(ogle-extract PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  # the vendored code keeps jak-project's own warnings to itself
  target_compile_options(ogle-extract PRIVATE -w)
  # the decompiler's code uses SSE4 / AVX intrinsics (crc32, tie unpacking), like jak-project
  target_compile_options(ogle-extract PRIVATE -mavx)
endif()
set_target_properties(ogle-extract PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# the game tables next to the executable
add_custom_command(TARGET ogle-extract POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/data"
          "${CMAKE_BINARY_DIR}/bin/ogle-extract-data")
