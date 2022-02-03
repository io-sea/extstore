
SET(extstore_crud_cache_LIB_SRCS
    extstore_crud_cache.c
)

add_library(extstore_crud_cache SHARED ${extstore_crud_cache_LIB_SRCS})
target_link_libraries(extstore_crud_cache ini_config dl)

install(TARGETS extstore_crud_cache DESTINATION lib)

find_package(PkgConfig REQUIRED)
pkg_check_modules(gmod REQUIRED IMPORTED_TARGET gmodule-2.0)
pkg_check_modules(json REQUIRED IMPORTED_TARGET jansson)

SET(objstore_cmd_LIB_SRCS
    objstore_cmd.c
    extstore_crud_cache_comm.h
    extstore_crud_cache_comm.c
)

add_library(objstore_cmd SHARED ${objstore_cmd_LIB_SRCS})
target_link_libraries(objstore_cmd ini_config PkgConfig::gmod PkgConfig::json curl)

install(TARGETS objstore_cmd DESTINATION lib)
