add_library(CARERuntime SHARED
    care.c
    dw.c
    errx.c
    handler.c
    udis.c
    util.c
)

find_library(LIBUDIS udis86)
message("libudis86: " ${LIBUDIS})

find_library(LIBDWARF dwarf)
message("libdwarf: " ${LIBDWARF})

find_library(LIBMHASH mhash)
message("libmhash: " ${LIBMHASH})

find_library(LIBFFI ffi)
message("libffi: " ${LIBFFI})

find_library(LIBUNWIND unwind)
message("libunwind: " ${LIBUNWIND})

find_library(LIBELF elf)
message("libelf: " ${LIBELF})

find_library(LIBZ z)
message("libz: " ${LIBZ})

link_directories(
    ${LLVM_LIBRARY_DIRS}
    ${CARE_SOURCE_DIR}/rtable
)
set(CMAKE_C_FLAGS "-g -DDEBUG")
target_link_libraries(CARERuntime ${LIBUDIS} ${LIBDWARF} ${LIBMHASH} ${LIBFFI} ${LIBUNWIND} ${LIBELF} ${LIBZ} rtable LLVMCore LLVMSupport)
