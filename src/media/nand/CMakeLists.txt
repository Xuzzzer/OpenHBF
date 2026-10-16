add_library(openhbx_media_nand STATIC
  die_domain.cpp
  nand_channel_slice.cpp
  nand_skeleton.cpp
  topology.cpp
)
target_include_directories(openhbx_media_nand PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(openhbx_media_nand PUBLIC openhbx_common openhbx_system openhbx_config openhbx_ras openhbx_warnings)
set_property(TARGET openhbx_media_nand PROPERTY OPENHBX_ROLE PRODUCTION_COMPONENT)
add_library(OpenHBX::media_nand ALIAS openhbx_media_nand)
