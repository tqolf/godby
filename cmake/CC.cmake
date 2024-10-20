macro (cc_set_policies)
  # the policy allows us to change options without caching
  cmake_policy(SET CMP0077 NEW)
  set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

  # the policy allows us to change set(CACHE) without caching
  if (POLICY CMP0126)
    cmake_policy(SET CMP0126 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0126 NEW)
  endif ()

  # The policy uses the download time for timestamp, instead of the timestamp in the archive. This allows for proper
  # rebuilds when a projects url changes
  if (POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)
  endif ()

  # the policy convert relative paths to absolute according to above rules
  cmake_policy(SET CMP0076 NEW)
  set(CMAKE_POLICY_DEFAULT_CMP0076 NEW)

  # the policy does not dereference variables or interpret keywords that have been quoted or bracketed.
  cmake_policy(SET CMP0054 NEW)
  set(CMAKE_POLICY_DEFAULT_CMP0054 NEW)
endmacro ()
cc_set_policies()

if(NOT WIN32)
  string(ASCII 27 Esc)
  set(CSI_Reset       "${Esc}[m")
  set(CSI_BOLD        "${Esc}[1m")
  set(CSI_Red         "${Esc}[31m")
  set(CSI_Green       "${Esc}[32m")
  set(CSI_Yellow      "${Esc}[33m")
  set(CSI_Blue        "${Esc}[34m")
  set(CSI_Magenta     "${Esc}[35m")
  set(CSI_Cyan        "${Esc}[36m")
  set(CSI_White       "${Esc}[37m")
  set(CSI_BoldRed     "${Esc}[1;31m")
  set(CSI_BoldGreen   "${Esc}[1;32m")
  set(CSI_BoldYellow  "${Esc}[1;33m")
  set(CSI_BoldBlue    "${Esc}[1;34m")
  set(CSI_BoldMagenta "${Esc}[1;35m")
  set(CSI_BoldCyan    "${Esc}[1;36m")
  set(CSI_BoldWhite   "${Esc}[1;37m")
endif()

# dpendency formats:
#   single package or target: pthread, your_cmake_target
#   path to precompiled library: /path/to/compied_library
#   single package can be found vai find_package: Boost::system
#   multiple package can be found vai find_package: Boost::system,filesystem
function(cc_add_dependencies target dependencies)
    # Find the position of "::" in the dependencies string
    string(FIND "${dependencies}" "::" pos)

    if (pos GREATER -1)
        # Separate the package name and the components part
        math(EXPR components_start "${pos} + 2")
        string(SUBSTRING "${dependencies}" 0 ${pos} package_name)
        string(SUBSTRING "${dependencies}" ${components_start} -1 components_str)

        # Handle the components list (split by commas or spaces)
        string(REPLACE "," ";" components_list "${components_str}")
        string(REPLACE " " ";" components_list "${components_list}")

        set(missing_components "")  # List to store missing components

        # Check if each component already exists as a target
        foreach(component IN LISTS components_list)
            if (NOT TARGET ${package_name}::${component})
                message(STATUS "Component ${package_name}::${component} not found, will attempt to find it.")
                list(APPEND missing_components ${component})  # Add missing component to the list
            else()
                message(STATUS "Package ${package_name}::${component} already found, skipping.")
            endif()
        endforeach()

        # If there are missing components, call find_package to locate them
        if (missing_components)
            message(STATUS "add_dependencies: ${package_name} with missing components: ${missing_components}")
            find_package(${package_name} REQUIRED COMPONENTS ${missing_components})
        endif()

        # Link all components (including already found and newly found)
        foreach(component IN LISTS components_list)
            target_link_libraries(${target} PUBLIC ${package_name}::${component})
        endforeach()
    else()
        # If no "::" is found, treat it as a single library
        if (NOT TARGET ${dependencies})
            message(STATUS "add_dependencies: ${dependencies}")
            find_package(${dependencies} QUIET)
        else()
            message(STATUS "Package ${dependencies} already found, skipping find_package.")
        endif()
        target_link_libraries(${target} PUBLIC ${dependencies})
    endif()
endfunction()

# HEADERS/SOURCES:
#   - Recursive: /path/to/(**.h, **.cc)
#   - Non-recursive: /path/to/(*.h, *.cc)
#   - Relative/Absolute: /path/to/(xx.h, xx.cc)
#
# OPTIONS: Compile Options
# LINK_OPTIONS: Link Options
#
# INCLUDES: Include Directories
#
# DEPENDENCIES: Dependencies of this target
#
# SANITIZE_OPTIONS: Sanitize Options
#   - DEB: Enables debugging information for source-level troubleshooting with debuggers
#   - CFI: Ensures control flow integrity to prevent function pointer or vtable hijacking
#   - GCOV: Provides test coverage analysis by generating statistics on tested code
#   - FUZZ: Uses random input generation to detect vulnerabilities or crashes
#   - MSAN: Detects uninitialized memory reads
#   - ASAN: Identifies memory errors like buffer overflows and use-after-free
#   - TSAN: Detects data races in multithreaded programs
#   - UBSAN: Identifies undefined behavior such as integer overflow or invalid type casts
function (cc_target type)
  set(singleOptions NAME)
  set(flags EXCLUDE_FROM_ALL AUTO_LANGUAGE_STANDARD STRICT_WARNINGS SMALL_FOOTPRINT)
  set(multipleOptions HEADERS SOURCES OPTIONS LINK_OPTIONS INCLUDES DEPENDENCIES SANITIZE_OPTIONS)

  string(TOUPPER "${type}" TYPE_UPPER)
  if ("${TYPE_UPPER}" STREQUAL "LIBRARY")
    list(APPEND singleOptions SHARED VISIBILITY ALWAYS_LINK)
  endif ()
  cmake_parse_arguments(CC "$flags" "${singleOptions}" "${multipleOptions}" "${ARGN}")

  set(ALL_TARGETS)
  set(MESSAGE_PREFIX "CC:${CC_NAME}")

  if ("${TYPE_UPPER}" STREQUAL "LIBRARY")
    if ("${CC_SHARED}" STREQUAL "")
      message(STATUS "${MESSAGE_PREFIX} - ${CSI_Blue}Both static and shared libraries${CSI_Reset}")
      add_library(${CC_NAME}-static STATIC)
      add_library(${CC_NAME}-shared SHARED)
      set_target_properties(${CC_NAME}-static PROPERTIES OUTPUT_NAME ${CC_NAME})
      set_target_properties(${CC_NAME}-shared PROPERTIES OUTPUT_NAME ${CC_NAME})
      add_library(${CC_NAME} ALIAS ${CC_NAME}-shared)
      list(APPEND ALL_TARGETS ${CC_NAME}-static ${CC_NAME}-shared)
    else ()
      if (NOT CC_SHARED)
        message(STATUS "${MESSAGE_PREFIX} - ${CSI_Blue}Only static library${CSI_Reset}")
        add_library(${CC_NAME}-static STATIC)
        set_target_properties(${CC_NAME}-static PROPERTIES OUTPUT_NAME ${CC_NAME})
        add_library(${CC_NAME} ALIAS ${CC_NAME}-static)
        list(APPEND ALL_TARGETS ${CC_NAME}-static)
      else ()
        message(STATUS "${MESSAGE_PREFIX} - ${CSI_Blue}Only shared library${CSI_Reset}")
        add_library(${CC_NAME}-shared SHARED)
        set_target_properties(${CC_NAME}-shared PROPERTIES OUTPUT_NAME ${CC_NAME})
        add_library(${CC_NAME} ALIAS ${CC_NAME}-shared)
        list(APPEND ALL_TARGETS ${CC_NAME}-shared)
      endif ()
    endif ()
    install(TARGETS ${ALL_TARGETS} DESTINATION lib)
  elseif ("${TYPE_UPPER}" STREQUAL "BINARY")
    message(STATUS "${MESSAGE_PREFIX} - ${CSI_Blue}Binary${CSI_Reset}")
    add_executable(${CC_NAME})
    list(APPEND ALL_TARGETS ${CC_NAME})
    install(TARGETS ${CC_NAME} DESTINATION bin)
  elseif ("${TYPE_UPPER}" STREQUAL "TEST")
    message(STATUS "${MESSAGE_PREFIX} - ${CSI_Blue}Binary for testing${CSI_Reset}")
    add_executable(${CC_NAME})
    list(APPEND ALL_TARGETS ${CC_NAME})
    add_test(${CC_NAME} ${CC_NAME})
    install(TARGETS ${CC_NAME} DESTINATION bin)
  else ()
    message(FATAL_ERROR "${CSI_BoldRed}Unknown type: ${TYPE_UPPER}${CSI_Reset}")
  endif ()

  if (NOT "${CC_VISIBILITY}" STREQUAL "")
    foreach (target IN LISTS ALL_TARGETS)
      set_target_properties(${target} PROPERTIES C_VISIBILITY_PRESET ${CC_VISIBILITY})
      set_target_properties(${target} PROPERTIES CXX_VISIBILITY_PRESET ${CC_VISIBILITY})
    endforeach ()
  endif ()

  if (CC_HEADERS)
    set(HEADER_FILES)
    foreach (HEADER ${CC_HEADERS})
      if (${HEADER} MATCHES ".*\\*\\*.*")
        set(MATCHED_HEADERS)
        file(GLOB_RECURSE MATCHED_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} ${HEADER})
        # message(STATUS "${MESSAGE_PREFIX}:${HEADER} - ${MATCHED_HEADERS}")
        list(APPEND HEADER_FILES ${MATCHED_HEADERS})
      elseif (${HEADER} MATCHES ".*\\*.*")
        set(MATCHED_HEADERS)
        file(GLOB MATCHED_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} ${HEADER})
        # message(STATUS "${MESSAGE_PREFIX}:${HEADER} - ${MATCHED_HEADERS}")
        list(APPEND HEADER_FILES ${MATCHED_HEADERS})
      else ()
        # message(STATUS "${MESSAGE_PREFIX}:${HEADER} - ${HEADER}")
        list(APPEND HEADER_FILES ${HEADER})
      endif ()
    endforeach ()
    list(LENGTH HEADER_FILES length)
    message(STATUS "${MESSAGE_PREFIX} - ${length} header files")
    install(FILES ${HEADER_FILES} DESTINATION include/${CC_NAME})
  endif ()

  if (CC_SOURCES)
    set(SOURCE_FILES)
    foreach (SOURCE ${CC_SOURCES})
      if (${SOURCE} MATCHES ".*\\*\\*.*")
        set(MATCHED_SOURCES)
        file(GLOB_RECURSE MATCHED_SOURCES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} ${SOURCE})
        # message(STATUS "${MESSAGE_PREFIX}:${SOURCE} - ${MATCHED_SOURCES}")
        foreach (target IN LISTS ALL_TARGETS)
          target_sources(${target} PRIVATE ${MATCHED_SOURCES})
        endforeach ()
        list(APPEND SOURCE_FILES ${MATCHED_SOURCES})
      elseif (${SOURCE} MATCHES ".*\\*.*")
        set(MATCHED_SOURCES)
        file(GLOB MATCHED_SOURCES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} ${SOURCE})
        # message(STATUS "${MESSAGE_PREFIX}:${SOURCE} - ${MATCHED_SOURCES}")
        foreach (target IN LISTS ALL_TARGETS)
          target_sources(${target} PRIVATE ${MATCHED_SOURCES})
        endforeach ()
        list(APPEND SOURCE_FILES ${MATCHED_SOURCES})
      else ()
        # message(STATUS "${MESSAGE_PREFIX}:${SOURCE} - ${SOURCE}")
        foreach (target IN LISTS ALL_TARGETS)
          target_sources(${target} PRIVATE ${SOURCE})
        endforeach ()
        list(APPEND SOURCE_FILES ${SOURCE})
      endif ()
    endforeach ()
    list(LENGTH SOURCE_FILES length)
    message(STATUS "${MESSAGE_PREFIX} - ${length} source files")
  endif ()

  if (CC_INCLUDES)
    foreach (target IN LISTS ALL_TARGETS)
      target_include_directories(${target} PUBLIC ${CC_INCLUDES})
    endforeach ()
    foreach (dir IN LISTS CC_INCLUDES)
      install(DIRECTORY ${dir} DESTINATION ${CMAKE_INSTALL_PREFIX})
    endforeach ()
  endif ()

  if (CC_OPTIONS)
    foreach (target IN LISTS ALL_TARGETS)
      target_compile_options(${target} PRIVATE ${CC_OPTIONS})
    endforeach ()
  endif ()

  if (CC_LINK_OPTIONS)
    foreach (target IN LISTS ALL_TARGETS)
      target_link_options(${target} PRIVATE ${CC_LINK_OPTIONS})
    endforeach ()
  endif ()

  if(CC_EXCLUDE_FROM_ALL)
    foreach (target IN LISTS ALL_TARGETS)
      set_property(TARGET ${target} PROPERTY EXCLUDE_FROM_ALL ON)
    endforeach ()
  endif()

  # Option: Whole achrive
  if (CC_ALWAYS_LINK)
    foreach (target IN LISTS ALL_TARGETS)
      set_property(TARGET ${target} PROPERTY LINK_BEHAVIOR WHOLE_ARCHIVE)
    endforeach ()
  endif ()
  foreach (depend ${CC_DEPENDENCIES})
    set(link_behavior)
    if (TARGET ${depend})
      # For non-cmake target, get_target_property will throw an error
      get_target_property(link_behavior ${depend} LINK_BEHAVIOR)
    endif ()
    if ("${link_behavior}" STREQUAL "WHOLE_ARCHIVE")
      message(STATUS "${MESSAGE_PREFIX}:${CSI_Yellow}${depend} Link whole archive${CSI_Reset}")
      # TODO(Oakley): fix this issue link whole-archive of static library will cause the multiple defined symbols error,
      # those also defined in libgcc. allow multiple definition is not good idea. it will cause constructors defined in
      # libraries execute more than twice times. build shared library can temporarily fix this problem

      foreach (target IN LISTS ALL_TARGETS)
        if (APPLE)
          target_link_libraries(${target} PRIVATE -Wl,-force_load ${depend})
        elseif (MSVC)
          # In MSVC, we will add whole archive in default.
          target_link_libraries(${target} PRIVATE -WHOLEARCHIVE:${depend})
        else ()
          # Assume everything else is like gcc
          target_link_libraries(${target} PRIVATE -Wl,--allow-multiple-definition)
          target_link_libraries(${target} PRIVATE -Wl,--whole-archive ${depend} -Wl,--no-whole-archive)
        endif ()
      endforeach ()
    else ()
      foreach (target IN LISTS ALL_TARGETS)
        cc_add_dependencies(${target} ${depend})
      endforeach ()
    endif ()
  endforeach ()
  if (CC_DEPENDENCIES)
    foreach (target IN LISTS ALL_TARGETS)
      message(STATUS "${MESSAGE_PREFIX}:${CSI_BoldBlue}dependencies ${CC_DEPENDENCIES}${CSI_Reset}")
    endforeach ()
  endif ()

  # Enable position-independent code defaultly. This is needed because some library targets are OBJECT libraries.
  message(STATUS "${MESSAGE_PREFIX} - ${CSI_Blue}Position Independent Code${CSI_Reset}")
  foreach (target IN LISTS ALL_TARGETS)
    set_property(TARGET ${target} PROPERTY POSITION_INDEPENDENT_CODE ON)
  endforeach ()

  # libunwind: a portable and efficient C programming interface (API) to determine the call-chain of a program
  if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
    find_package(PkgConfig)
    if (PkgConfig_FOUND)
      PKG_CHECK_MODULES(LIBUNWIND libunwind-generic)
      if (LIBUNWIND_FOUND)
        foreach (target IN LISTS ALL_TARGETS)
          target_compile_definitions(${target} PRIVATE -DHAVE_LIBUNWIND)
        endforeach ()
      else ()
        message(STATUS "${CSI_Yellow}libunwind not found. Disabling unwind tests.${CSI_Reset}")
      endif ()
    else ()
      message(STATUS "${CSI_Yellow}pkgconfig not found. Disabling unwind tests.${CSI_Reset}")
    endif ()
  endif ()

  if (CC_STRICT_WARNINGS)
    message(STATUS "${MESSAGE_PREFIX} - strict warning")
    # Note clang-cl is odd and sets both CLANG and MSVC. We base our configuration primarily on our normal Clang one.

    foreach (target IN LISTS ALL_TARGETS)
      target_compile_options(
        ${target}
        PRIVATE $<$<COMPILE_LANGUAGE:C,CXX,ASM>:-Wformat=2
                -Wsign-compare
                -Wmissing-field-initializers
                -Wwrite-strings
                -Wvla
                -Wcast-align
                -Wcast-qual
                -Wswitch-enum
                -Wundef
                -Wdouble-promotion
                -Wdate-time
                -Wfloat-equal
                -fno-strict-aliasing
                -pipe
                -Wunused-const-variable
                -Wall
                -Wextra
                -fno-common
                -fvisibility=default>
      )

      target_compile_options(
        ${target}
        PRIVATE $<$<AND:$<COMPILE_LANGUAGE:C,CXX,ASM>,$<CXX_COMPILER_ID:GNUCC,GNUCXX>>:-freg-struct-return
                -Wtrampolines
                -Wl,-z,relro,-z,now
                -fstack-protector-strong
                -fdata-sections
                -ffunction-sections
                -Wl,--gc-sections
                -Wmissing-format-attribute
                -Wstrict-overflow=2
                -Wswitch-default
                -Wconversion
                -Wunused
                -Wpointer-arith>
      )

      if (CMAKE_COMPILER_IS_GNUCC)
        target_compile_options(
          ${target}
          PRIVATE $<$<VERSION_GREATER:$<C_COMPILER_VERSION>,4.3.0>:-Wlogical-op>
                  $<$<VERSION_GREATER:$<C_COMPILER_VERSION>,4.8.0>:-Wno-array-bounds>
                  # GCC (at least 4.8.4) has a bug where it'll find unreachable free() calls and declare that the code is
                  # trying to free a stack pointer.
                  $<$<VERSION_GREATER:$<C_COMPILER_VERSION>,4.8.4>:-Wno-free-nonheap-object>
                  $<$<VERSION_GREATER:$<C_COMPILER_VERSION>,6.0.0>:-Wduplicated-cond -Wnull-dereference>
                  $<$<VERSION_GREATER:$<C_COMPILER_VERSION>,7.0.0>:-Wduplicated-branches -Wrestrict>
        )

        # shared or module
        target_link_options(${target} PRIVATE $<$<NOT:$<BOOL:${APPLE}>>:-Wl,--fatal-warnings -Wl,--no-undefined>)
      endif ()

      if (${CMAKE_CXX_COMPILER_ID} MATCHES "Clang")
        target_compile_options(
          ${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX,ASM>:-Wmissing-variable-declarations -Wcomma
                                    -Wused-but-marked-unused -Wnewline-eof -fcolor-diagnostics>
        )
        target_compile_options(
          ${target} PRIVATE $<$<VERSION_GREATER:$<C_COMPILER_VERSION>,7.0.0>:-Wimplicit-fallthrough>
        )
      endif ()

      target_compile_options(
        ${target} PRIVATE $<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang,GNUCC,GNUCXX>: -Wmissing-prototypes
                                  -Wold-style-definition -Wstrict-prototypes>
      )
      target_compile_options(
        ${target} PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNUCC,GNUCXX>:-Wmissing-declarations
                                  -Weffc++>
      )

      # In GCC, -Wmissing-declarations is the C++ spelling of -Wmissing-prototypes and using the wrong one is an error. In
      # Clang, -Wmissing-prototypes is the spelling for both and -Wmissing-declarations is some other warning.
      #
      # https://gcc.gnu.org/onlinedocs/gcc-7.1.0/gcc/Warning-Options.html#Warning-Options
      # https://clang.llvm.org/docs/DiagnosticsReference.html#wmissing-prototypes
      # https://clang.llvm.org/docs/DiagnosticsReference.html#wmissing-declarations
      target_compile_options(
        ${target} PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang>:-Wmissing-prototypes>
      )

      target_compile_options(${target} PRIVATE $<$<COMPILE_LANG_AND_ID:C,GNUCC>:-Wc++-compat>)
      target_compile_options(
        ${target}
        PRIVATE $<$<VERSION_GREATER:$<C_COMPILER_VERSION>,4.7.99>:-Wshadow>
                $<$<VERSION_GREATER:$<CXX_COMPILER_VERSION>,4.7.99>:-Wshadow>
                # $<$<VERSION_GREATER:$<ASM_COMPILER_VERSION>,4.7.99>:-Wshadow>
      )
    endforeach()
  elseif (NOT "${TYPE_UPPER}" STREQUAL "TEST")
    message(STATUS "${MESSAGE_PREFIX} - No strict warnings")
    foreach (target IN LISTS ALL_TARGETS)
      target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX,ASM>: -Wall -Wextra>)
    endforeach ()
  else ()
    message(STATUS "${MESSAGE_PREFIX} - No warnings for test")
    foreach (target IN LISTS ALL_TARGETS)
      target_compile_options(${target} PRIVATE "$<$<CXX_COMPILER_ID:GNU,Clang>:-w>")
    endforeach ()
  endif ()

  if (CC_SMALL_FOOTPRINT)
    message(STATUS "${MESSAGE_PREFIX} - ${CSI_Blue}Small footprint${CSI_Reset}")
    foreach (target IN LISTS ALL_TARGETS)
      target_compile_options(
        ${target}
        PRIVATE -Os
                $<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang>:-flto=thin>
                $<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang>:-flto=thin>
                $<$<COMPILE_LANG_AND_ID:ASM,AppleClang,Clang>:-flto=thin>
                $<$<COMPILE_LANG_AND_ID:C,GNUCC,GNUCXX>:--specs=nosys.specs
                --specs=nano.specs>
                $<$<COMPILE_LANG_AND_ID:CXX,GNUCC,GNUCXX>:--specs=nosys.specs
                --specs=nano.specs>
                $<$<COMPILE_LANG_AND_ID:ASM,GNUCC,GNUCXX>:--specs=nosys.specs
                --specs=nano.specs>
      )
    endforeach ()
  endif ()

  foreach (target IN LISTS ALL_TARGETS)
    # target_compile_options(${target} PRIVATE $<IF:$<CONFIG:Debug>,-O0 -g3,-O2 -g>)
    target_compile_definitions(${target} PRIVATE $<IF:$<CONFIG:Debug>,__DEBUG__,__RELEASE__ NDEBUG>)

    target_compile_options(${target} PRIVATE -fomit-frame-pointer)
  endforeach ()

  if (SANITIZE_OPTIONS_FUZZ)
    message(STATUS "${MESSAGE_PREFIX} - ${CSI_Blue}Fuzz enabled${CSI_Reset}")
    if (NOT CMAKE_COMPILER_IS_CLANG)
      message(FATAL_ERROR "You need to build with Clang for fuzzing to work")
    endif ()

    if (CMAKE_C_COMPILER_VERSION VERSION_LESS "6.0.0")
      message(FATAL_ERROR "You need Clang ≥ 6.0.0")
    endif ()

    foreach (target IN LISTS ALL_TARGETS)
      target_compile_definitions(${target} PRIVATE -DUNSAFE_DETERMINISTIC_MODE)
    endforeach ()
    set(RUNNER_CC "-deterministic")

    if (NOT SANITIZE_OPTIONS_NO_FUZZER_MODE)
      foreach (target IN LISTS ALL_TARGETS)
        target_compile_definitions(${target} PRIVATE -DUNSAFE_FUZZER_MODE)
      endforeach ()
      set(RUNNER_CC ${RUNNER_CC} "-fuzzer" "-shim-config" "fuzzer_mode.json")
    endif ()

    foreach (target IN LISTS ALL_TARGETS)
      target_compile_options(
        ${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:-fsanitize=address,fuzzer-no-link
                                  -fsanitize-coverage=edge,indirect-calls>
      )
    endforeach ()
  endif ()

  if (SANITIZE_OPTIONS_MSAN)
    if (SANITIZE_OPTIONS_ASAN)
      message(FATAL_ERROR "${CSI_BoldRed}ASAN and MSAN are mutually exclusive${CSI_Reset}")
    endif ()

    foreach (target IN LISTS ALL_TARGETS)
      target_compile_options(
        ${target}
        PRIVATE $<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang>:-fsanitize=memory -fsanitize-memory-track-origins
                -fno-omit-frame-pointer> $<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang>:-fsanitize=memory
                -fsanitize-memory-track-origins -fno-omit-frame-pointer>
      )
    endforeach ()
  endif ()

  if (SANITIZE_OPTIONS_ASAN)
    if (SANITIZE_OPTIONS_MSAN)
      message(FATAL_ERROR "${CSI_BoldRed}ASAN and MSAN are mutually exclusive${CSI_Reset}")
    endif ()

    foreach (target IN LISTS ALL_TARGETS)
      target_compile_options(
        ${target}
        PRIVATE $<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang>:-fsanitize=address -fsanitize-address-use-after-scope
                -fno-omit-frame-pointer> $<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang>:-fsanitize=address
                -fsanitize-address-use-after-scope -fno-omit-frame-pointer>
      )
      target_link_options(
        ${target} PRIVATE $<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang>:-fsanitize=address
        -fsanitize-address-use-after-scope -fno-omit-frame-pointer>
        $<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang>:-fsanitize=address -fsanitize-address-use-after-scope
        -fno-omit-frame-pointer>
      )
    endforeach ()
  endif ()

  if (CC_SANITIZE_OPTIONS)
    foreach (check IN LISTS CC_SANITIZE_OPTIONS)
      string(TOUPPER "${check}" CHECK)
      set(SANITIZE_OPTIONS_${CHECK} ON FORCE)
    endforeach ()
  endif ()

  if (SANITIZE_OPTIONS_DEB)
    foreach (target IN LISTS ALL_TARGETS)
      target_compile_options(${target} PRIVATE $<$<COMPILE_LANG:C,CXX>:-z,noexecstack>)
    endforeach ()
  endif ()

  # ROP(Return-oriented Programming) Attack
  if (SANITIZE_OPTIONS_CFI)
    if (NOT CMAKE_COMPILER_IS_CLANG)
      message(FATAL_ERROR "${CSI_BoldRed}Cannot enable CFI unless using Clang${CSI_Reset}")
    endif ()

    foreach (target IN LISTS ALL_TARGETS)
      target_compile_options(
        ${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:-fsanitize=cfi -fno-sanitize-trap=cfi -flto=thin>
      )

      # We use Chromium's copy of clang, which requires -fuse-ld=lld if building with -flto. That, in turn, can't handle
      # -ggdb.
      target_link_options($<$<STREQUAL:$<TARGET_PROPERTY:${target},TYPE>,"EXECUTABLE">:-fuse-ld=lld>)
    endforeach ()
    # FIXME
    string(REPLACE "-ggdb" "-g" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
    string(REPLACE "-ggdb" "-g" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    # -flto causes object files to contain LLVM bitcode. Mixing those with assembly output in the same static library
    # breaks the linker.
    set(NO_ASM ON FORCE)
  endif ()

  if (SANITIZE_OPTIONS_TSAN)
    if (NOT CMAKE_COMPILER_IS_CLANG)
      message(FATAL_ERROR "${CSI_BoldRed}Cannot enable TSAN unless using Clang${CSI_Reset}")
    endif ()

    foreach (target IN LISTS ALL_TARGETS)
      target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:-fsanitize=thread>)

      target_link_options($<$<STREQUAL:$<TARGET_PROPERTY:${target},TYPE>,"EXECUTABLE">:-fsanitize=thread>)
    endforeach ()
  endif ()

  if (SANITIZE_OPTIONS_UBSAN)
    if (NOT CMAKE_COMPILER_IS_CLANG)
      message(FATAL_ERROR "${CSI_BoldRed}Cannot enable UBSAN unless using Clang${CSI_Reset}")
    endif ()

    foreach (target IN LISTS ALL_TARGETS)
      target_compile_options(
        ${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:-fsanitize=undefined -fsanitize=float-divide-by-zero
                                  -fsanitize=float-cast-overflow -fsanitize=integer>
      )

      target_link_options(
        $<$<STREQUAL:$<TARGET_PROPERTY:${target},TYPE>,"EXECUTABLE">:-fsanitize=undefined
        -fsanitize=float-divide-by-zero -fsanitize=float-cast-overflow -fsanitize=integer>
      )

      if (NOT SANITIZE_OPTIONS_UBSAN_RECOVER)
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:-fno-sanitize-recover=undefined>)

        target_link_options(
          $<$<STREQUAL:$<TARGET_PROPERTY:${target},TYPE>,"EXECUTABLE">: -fno-sanitize-recover=undefined>
        )
      endif ()
    endforeach ()
  endif ()

  # Coverage
  if (SANITIZE_OPTIONS_GCOV)
    foreach (target IN LISTS ALL_TARGETS)
      target_link_libraries(${target} PRIVATE gcov)
      target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:-fprofile-arcs -ftest-coverage>)
    endforeach ()
  endif ()

  if (CC_AUTO_LANGUAGE_STANDARD)
    set(C_STANDARDS -std=gnu18 -std=c18 -std=gnu11 -std=c11 -std=gnu99 -std=c99)
    INCLUDE(CheckCCompilerFlag)
    foreach (std ${C_STANDARDS})
      CHECK_C_COMPILER_FLAG(${std} supported_${std})
      if (supported_${std})
        foreach (target IN LISTS ALL_TARGETS)
          target_compile_options(
            ${target} PUBLIC $<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang,GNUCC,GNUCXX>:${std}>
          )
        endforeach ()
        break()
      endif ()
    endforeach ()

    set(CXX_STANDARDS -std=gnu++2a -std=c++2a -std=gnu++1z -std=c++1z -std=gnu++14 -std=c++14 -std=gnu++11 -std=c++11)
    INCLUDE(CheckCXXCompilerFlag)
    foreach (std ${CXX_STANDARDS})
      CHECK_CXX_COMPILER_FLAG(${std} supported_${std})
      if (supported_${std})
        foreach (target IN LISTS ALL_TARGETS)
          target_compile_options(
            ${target} PUBLIC $<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNUCC,GNUCXX>:${std}>
          )
        endforeach ()
        break()
      endif ()
    endforeach ()
  else ()
    foreach (target IN LISTS ALL_TARGETS)
      target_compile_options(
        ${target} PUBLIC $<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang,GNUCC,GNUCXX>:-std=gnu11>
                                  $<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNUCC,GNUCXX>:-std=gnu++17>
      )
    endforeach ()
  endif ()

  if (CC_LINK_PTHREAD)
    find_package(Threads)
    if (Threads_FOUND)
      foreach (target IN LISTS ALL_TARGETS)
        target_link_libraries(${target} PUBLIC ${CMAKE_THREAD_LIBS_INIT} ${CMAKE_DL_LIBS})
      endforeach ()
    endif ()
  endif ()

  # redefine __FILE__ after stripping project dir
  foreach (target IN LISTS ALL_TARGETS)
    target_compile_options(${target} PRIVATE -Wno-builtin-macro-redefined)
  endforeach ()
  # Get source files of target
  get_target_property(source_files ${CC_NAME} SOURCES)
  foreach (srcfile ${source_files})
    # Get compile definitions in source file
    GET_PROPERTY(defs SOURCE "${srcfile}" PROPERTY COMPILE_DEFINITIONS)
    # Get absolute path of source file
    GET_FILENAME_COMPONENT(filepath "${srcfile}" ABSOLUTE)
    # Trim leading dir
    string(FIND "${filepath}" "${CMAKE_BINARY_DIR}" pos)
    if (${pos} EQUAL 0)
      file(RELATIVE_PATH relpath ${CMAKE_BINARY_DIR} ${filepath})
    else ()
      file(RELATIVE_PATH relpath ${CMAKE_SOURCE_DIR} ${filepath})
    endif ()
    # Add __FILE__ definition to compile definitions
    list(APPEND defs "__FILE__=\"${relpath}\"")
    # Set compile definitions to property
    set_property(SOURCE "${srcfile}" PROPERTY COMPILE_DEFINITIONS ${defs})
  endforeach ()

  # cmake-format: off
  add_custom_target(
    ${CC_NAME}_gcov
    COMMAND ${CMAKE_COMMAND} -E make_directory reports/coverage
    COMMAND ${CMAKE_MAKE_PROGRAM} test
    COMMAND echo "Generating coverage reports ..."
    COMMAND gcovr -r ${CMAKE_SOURCE_DIR} --html --html-details
            ${CMAKE_BINARY_DIR}/reports/coverage/full.html
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
  # cmake-format: on
endfunction ()

function(cc_select target)
  add_library(${target} INTERFACE)

  list(LENGTH ARGN length)
  if(length GREATER 0)
    math(EXPR length "${length} - 1")
    foreach(key RANGE 0 ${length} 2)
      math(EXPR value "${key} + 1")
      list(GET ARGN ${key} condition)
      list(GET ARGN ${value} implementation)

      if((${condition} STREQUAL "DEFAULT") OR (${${condition}}))
        message(STATUS "${CSI_BoldRed}Using ${library_name} = ${implementation}${CSI_Reset}")
        target_link_libraries(${library_name} INTERFACE ${implementation})
        return()
      endif()
    endforeach()
  endif()
  message(FATAL_ERROR "${CSI_BoldRed}Could not find implementation for ${library_name}${CSI_Reset}")
endfunction()

function(cc_source_if target condition)
  if (${condition})
    target_sources(${target} PRIVATE ${ARGN})
  endif ()
endfunction ()

function(cc_alias AliasTarget ActualTarget)
  if(NOT TARGET ${AliasTarget})
    add_library(${AliasTarget} ALIAS ${ActualTarget})
  endif()
endfunction()

function (cc_library)
  cc_target(LIBRARY ${ARGV})
endfunction ()

function (cc_binary)
  cc_target(BINARY ${ARGV})
endfunction ()

function (cc_test)
  cc_target(TEST ${ARGV})
endfunction ()
