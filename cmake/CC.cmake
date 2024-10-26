cmake_minimum_required(VERSION 3.15)

# CSI escape sequences
if (NOT WIN32)
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
endif ()

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

function (cc_glob OUTPUTS PATTERNS)
  if (NOT ${PATTERNS})
    return()
  endif ()

  set(options EXCLUDE)
  set(oneValueArgs)
  set(multiValueArgs)

  # Parse additional options
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  set(MATCHED_FILES)
  # Loop through each pattern in the provided PATTERNS list
  foreach (pattern ${${PATTERNS}})
    # Perform recursive search if pattern contains '**'
    if (${pattern} MATCHES ".*\\*\\*.*")
      file(GLOB_RECURSE MATCHED_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} ${pattern})
    # Perform single-directory search if pattern contains '*'
    elseif (${pattern} MATCHES ".*\\*.*")
      file(GLOB MATCHED_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} ${pattern})
    # If no glob pattern is detected, directly append the file
    elseif (IS_DIRECTORY ${pattern})
      file(GLOB_RECURSE MATCHED_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} ${pattern}/*.*)
    else ()
      list(APPEND MATCHED_FILES ${pattern})
    endif()
  endforeach()

  if (ARG_EXCLUDE)
    list(REMOVE_ITEM ${OUTPUTS} ${MATCHED_FILES})
  else()
    list(APPEND ${OUTPUTS} ${MATCHED_FILES})
  endif()

  # Set the OUTPUTS variable in the caller's scope to preserve modifications
  set(${OUTPUTS} ${${OUTPUTS}} PARENT_SCOPE)
endfunction()

macro (cc_get_namespace variable)
  get_directory_property(__PARENT_DIR__ PARENT_DIRECTORY)
  get_property(${variable} DIRECTORY ${__PARENT_DIR__} PROPERTY __CURRENT_NAMESPACE__)
  if (${variable} STREQUAL "")
    message(FATAL_ERROR "Can not get namespace for ${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
endmacro ()

function(cc_set_namespace)
  set(options ROOT)
  set(oneValueArgs HINT)
  set(multiValueArgs)
  cmake_parse_arguments(cc "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGV})

  # Get the unparsed arguments as the namespace name
  set(namespace "${cc_UNPARSED_ARGUMENTS}")

  if(cc_ROOT)
    # If 'ROOT' option is used, ensure a namespace is provided
    if(NOT namespace)
      message(FATAL_ERROR "${cc_HINT}When using the ROOT option, you must provide a root namespace.")
    endif()
    # Set the root namespace for the current directory
    set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY __CURRENT_NAMESPACE__ "${namespace}")
  else()
    cc_get_namespace(__PARENT_NAMESPACE__)

    if(namespace)
      # Use the provided namespace
      set(current_namespace "${namespace}")
    else()
      # If no namespace is provided, use the current directory name
      get_filename_component(current_namespace "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
    endif()

    if(__PARENT_NAMESPACE__)
      # Combine the parent namespace and current namespace
      set(full_namespace "${__PARENT_NAMESPACE__}::${current_namespace}")
    else()
      # Use the current namespace as the full namespace
      set(full_namespace "${current_namespace}")
    endif()

    # Set the namespace property for the current directory
    set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY __CURRENT_NAMESPACE__ "${full_namespace}")
    message(STATUS "${cc_HINT}Using namespace: ${full_namespace}")
  endif()
endfunction()

# dpendency formats:
#   single package or target: pthread, your_cmake_target
#   path to precompiled library: /path/to/compied_library
#   single package can be found via find_package: Boost::system
#   multiple package can be found via find_package: Boost::system,filesystem
function (cc_deps target dependencies)
  # Find the position of "::" in the dependencies string
  string(FIND "${dependencies}" "::" pos)

  set(linkage_type "PUBLIC")
  get_target_property(TARGET_TYPE ${target} TYPE)
  if ("${TARGET_TYPE}" STREQUAL "INTERFACE_LIBRARY")
    set(linkage_type "INTERFACE")
  endif ()

  if (pos GREATER -1)
    # Separate the package name and the components part
    math(EXPR cstart "${pos} + 2")
    string(SUBSTRING "${dependencies}" 0 ${pos} package_name)
    string(SUBSTRING "${dependencies}" ${cstart} -1 components_str)

    # Handle the components list (split by commas or spaces)
    string(REPLACE "," ";" components_list "${components_str}")
    string(REPLACE " " ";" components_list "${components_list}")

    # List to store missing components
    set(missing_components) 

    # Check if each component already exists as a target
    foreach (component IN LISTS components_list)
      if (NOT TARGET ${package_name}::${component})
          message(STATUS "Component ${package_name}::${component} not found, will attempt to find it.")
          list(APPEND missing_components ${component})  # Add missing component to the list
      else ()
          message(STATUS "Package ${package_name}::${component} already found, skipping.")
      endif ()
    endforeach ()

    # If there are missing components, call find_package to locate them
    if (missing_components)
      message(STATUS "add_dependencies: ${package_name} with missing components: ${missing_components}")
      find_package(${package_name} QUIET COMPONENTS ${missing_components})
    endif ()

    # Link all components (including already found and newly found)
    foreach (component IN LISTS components_list)
      set(link_behavior)
      if (TARGET ${package_name}::${component})
        # For non-cmake target, get_target_property will throw an error
        get_target_property(link_behavior ${package_name}::${component} LINK_BEHAVIOR)

        if ("${link_behavior}" STREQUAL "WHOLE_ARCHIVE")
          message(STATUS "${hint_prefix}:${CSI_Yellow}${depend} Link whole archive${CSI_Reset}")
          # TODO(Oakley): fix this issue link whole-archive of static library will cause the multiple defined symbols error,
          # those also defined in libgcc. allow multiple definition is not good idea. it will cause constructors defined in
          # libraries execute more than twice times. build shared library can temporarily fix this problem
          if (APPLE)
            target_link_libraries(${target} ${linkage_type} -Wl,-force_load ${depend})
          elseif (MSVC)
            # In MSVC, we will add whole archive in default.
            target_link_libraries(${target} ${linkage_type} -WHOLEARCHIVE:${depend})
          else ()
            # Assume everything else is like gcc
            target_link_libraries(${target} ${linkage_type} -Wl,--allow-multiple-definition)
            target_link_libraries(${target} ${linkage_type} -Wl,--whole-archive ${depend} -Wl,--no-whole-archive)
          endif ()
        else ()
          target_link_libraries(${target} ${linkage_type} ${package_name}::${component})
        endif ()
      else ()
        target_link_libraries(${target} ${linkage_type} ${package_name}::${component})
      endif ()
    endforeach ()
  else ()
    # If no "::" is found, treat it as a single library
    if (NOT TARGET ${dependencies})
      message(STATUS "add_dependencies: ${dependencies}")
      find_package(${dependencies} QUIET)
    else ()
      message(STATUS "Package ${dependencies} already found, skipping find_package.")
    endif ()

    set(link_behavior)
    if (TARGET ${dependencies})
      # For non-cmake target, get_target_property will throw an error
      get_target_property(link_behavior ${dependencies} LINK_BEHAVIOR)

      if ("${link_behavior}" STREQUAL "WHOLE_ARCHIVE")
        message(STATUS "${hint_prefix}:${CSI_Yellow}${depend} Link whole archive${CSI_Reset}")
        # TODO(Oakley): fix this issue link whole-archive of static library will cause the multiple defined symbols error,
        # those also defined in libgcc. allow multiple definition is not good idea. it will cause constructors defined in
        # libraries execute more than twice times. build shared library can temporarily fix this problem
        if (APPLE)
          target_link_libraries(${target} ${linkage_type} -Wl,-force_load ${depend})
        elseif (MSVC)
          # In MSVC, we will add whole archive in default.
          target_link_libraries(${target} ${linkage_type} -WHOLEARCHIVE:${depend})
        else ()
          # Assume everything else is like gcc
          target_link_libraries(${target} ${linkage_type} -Wl,--allow-multiple-definition)
          target_link_libraries(${target} ${linkage_type} -Wl,--whole-archive ${depend} -Wl,--no-whole-archive)
        endif ()
      else ()
        target_link_libraries(${target} ${linkage_type} ${dependencies})
      endif ()
    else ()
      target_link_libraries(${target} ${linkage_type} ${dependencies})
    endif ()
  endif ()
endfunction ()

# Enable features for a specific target from global variables
function (cc_check_features target)
    # Iterate over the list of possible features (DEB, CFI, GCOV, etc.)
    foreach (feature IN ITEMS DEB CFI GCOV FUZZ MSAN TSAN ASAN UBSAN)
        # Construct the global variable name, e.g., ENABLED_DEB, ENABLED_CFI, etc.
        set(enabled_list_var "ENABLED_${feature}")

        # Ensure the global variable exists, if not, assign it an empty list by default
        if (NOT DEFINED ${enabled_list_var})
            set(${enabled_list_var} "")
        endif ()

        # Split the list of targets (in ENABLED_*) by commas, semicolons, and spaces
        string(REPLACE "," ";" ${enabled_list_var} "${${enabled_list_var}}")

        # Check if the target name exists in the ENABLED_* list
        list(FIND ${enabled_list_var} ${target} target_index)
        if (target_index GREATER -1)
            # If the target is found in the list, enable the corresponding FEATURE_*
            set(FEATURE_${feature} ON PARENT_SCOPE)
        else ()
            # Otherwise, disable the corresponding FEATURE_*
            set(FEATURE_${feature} OFF PARENT_SCOPE)
        endif ()
    endforeach ()
endfunction ()

function (cc_warnings)
  set(options)
  set(oneValueArgs)
  set(multiValueArgs TARGETS)
  cmake_parse_arguments(cc "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGV})

  foreach (target IN LISTS cc_TARGETS)
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
  endforeach ()
endfunction()

function (cc_small_footprint)
  set(options)
  set(oneValueArgs)
  set(multiValueArgs TARGETS)
  cmake_parse_arguments(cc "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGV})

  foreach (target IN LISTS cc_TARGETS)
    message(STATUS "CC:${target} - ${CSI_Blue}Small footprint${CSI_Reset}")
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
endfunction ()

# HEADERS/SOURCES:
#   - Recursive: /path/to/(**.h, **.cc)
#   - Non-recursive: /path/to/(*.h, *.cc)
#   - Relative/Absolute: /path/to/(xx.h, xx.cc)
#
# OPTIONS: Compile Options
# LINK_OPTIONS: Link Options
#
# INCLUDES: Include Directories, will be installed to ${CMAKE_INSTALL_PREFIX}/include
#
# DEPENDENCIES: Dependencies of this target
#
# FEATURES: Sanitize Options and Features
#   - DEB: Enables debugging information for source-level troubleshooting with debuggers
#   - CFI: Ensures control flow integrity to prevent function pointer or vtable hijacking
#   - GCOV: Provides test coverage analysis by generating statistics on tested code
#   - FUZZ: Uses random input generation to detect vulnerabilities or crashes
#   - MSAN: Detects uninitialized memory reads
#   - ASAN: Identifies memory errors like buffer overflows and use-after-free
#   - TSAN: Detects data races in multithreaded programs
#   - UBSAN: Identifies undefined behavior such as integer overflow or invalid type casts
function (cc_targets type)
  set(options INTERFACE EXCLUDE_FROM_ALL)
  set(oneValueArgs NAME NAMESPACE OUTPUT_NAME)
  set(multiValueArgs SOURCES OPTIONS DEFINITIONS LINK_OPTIONS INCLUDES DEPENDENCIES FEATURES EXCLUDE_SOURCES)

  string(TOUPPER "${type}" TYPE_UPPER)
  if (cc_INTERFACE OR "${TYPE_UPPER}" STREQUAL "LIBRARY")
    list(APPEND oneValueArgs SHARED ALIAS VISIBILITY ALWAYS_LINK LINK_PTHREAD)
    list(APPEND multiValueArgs HEADERS EXCLUDE_HEADERS HEADERS_BASE_DIR)
  endif ()
  cmake_parse_arguments(cc "$options" "${oneValueArgs}" "${multiValueArgs}" "${ARGN}")

  get_filename_component(CURRENT_DIR ${CMAKE_CURRENT_SOURCE_DIR} NAME)
  cc_get_namespace(CURRENT_SUPERIOR_NAMESPACE)
  if (NOT cc_NAME)
    if (cc_NAMESPACE)
      set(cc_NAMESPACE ${CURRENT_DIR})
    endif ()
    cc_set_namespace(${cc_NAMESPACE})
    if (CURRENT_SUPERIOR_NAMESPACE)
      set(cc_NAME ${CURRENT_SUPERIOR_NAMESPACE}::${cc_NAMESPACE})
    else ()
      set(cc_NAME ${cc_NAMESPACE})
    endif ()
    string(REPLACE "::" "_" cc_OUTPUTNAME ${cc_NAME})
  endif ()
  if (NOT cc_ALIAS)
    if (CURRENT_SUPERIOR_NAMESPACE)
      set(cc_ALIAS ${CURRENT_SUPERIOR_NAMESPACE}::${cc_NAMESPACE})
    else()
      set(cc_ALIAS ${cc_NAME})
    endif ()
  endif ()
  if (NOT cc_OUTPUTNAME)
    string(REPLACE "::" "_" cc_OUTPUTNAME ${cc_NAME})
  endif ()

  set(all_targets)
  set(all_targets_not_interface)
  set(hint_prefix "CC:${cc_NAME}")

  set(SOURCE_FILES)
  cc_glob(SOURCE_FILES cc_SOURCES)
  cc_glob(SOURCE_FILES cc_EXCLUDE_SOURCES EXCLUDE)

  cc_set_namespace(${cc_NAMESPACE} HINT "${hint_prefix} - ")
  if ("${TYPE_UPPER}" STREQUAL "LIBRARY")
    set(enable_shared_target OFF)
    set(enable_static_target OFF)
    set(enable_interface_target OFF)
    if ("${cc_SHARED}" STREQUAL "")
      set(enable_shared_target ON)
      set(enable_static_target ON)
    elseif (cc_SHARED)
      set(enable_shared_target ON)
    else ()
      set(enable_static_target ON)
    endif ()
    if (NOT SOURCE_FILES OR cc_INTERFACE)
      if (NOT SOURCE_FILES AND (enable_shared_target OR enable_static_target))
        message(STATUS "{hint_prefix} - ${CSI_Yellow}No source files, add interface only.${CSI_Reset}")
      endif ()
      set(enable_shared_target OFF)
      set(enable_static_target OFF)
      set(enable_interface_target ON)
    endif ()

    if (enable_shared_target)
      add_library(${cc_NAME}-shared SHARED)
      add_library(${cc_ALIAS} ALIAS ${cc_NAME}-shared)
      set_property(TARGET ${cc_NAME}-shared PROPERTY EXPORT_NAME ${cc_ALIAS})
      set_target_properties(${cc_NAME}-shared PROPERTIES OUTPUT_NAME ${cc_OUTPUTNAME})

      list(APPEND all_targets ${cc_NAME}-shared)
      list(APPEND all_targets_not_interface ${cc_NAME}-shared)
    endif ()

    if (enable_static_target)
      add_library(${cc_NAME}-static STATIC)
      if (NOT TARGET ${cc_ALIAS})
        add_library(${cc_ALIAS} ALIAS ${cc_NAME}-static)
        set_property(TARGET ${cc_NAME}-static PROPERTY EXPORT_NAME ${cc_ALIAS})
      endif ()
      set_target_properties(${cc_NAME}-static PROPERTIES OUTPUT_NAME ${cc_OUTPUTNAME})

      list(APPEND all_targets ${cc_NAME}-static)
      list(APPEND all_targets_not_interface ${cc_NAME}-static)
    endif ()

    if (enable_interface_target)
      add_library(${cc_NAME}-interface INTERFACE)
      if (NOT TARGET ${cc_ALIAS})
        add_library(${cc_ALIAS} ALIAS ${cc_NAME}-interface)
        set_property(TARGET ${cc_NAME}-interface PROPERTY EXPORT_NAME ${cc_ALIAS})
      endif ()
      list(APPEND all_targets ${cc_NAME}-interface)
    endif ()
    install(TARGETS ${all_targets} DESTINATION lib)
  elseif ("${TYPE_UPPER}" STREQUAL "BINARY")
    message(STATUS "${hint_prefix} - ${CSI_Blue}Binary${CSI_Reset}")
    add_executable(${cc_NAME})
    list(APPEND all_targets ${cc_NAME})
    list(APPEND all_targets_not_interface ${cc_NAME})
    install(TARGETS ${cc_NAME} DESTINATION bin)
  elseif ("${TYPE_UPPER}" STREQUAL "TEST")
    message(STATUS "${hint_prefix} - ${CSI_Blue}Testing Binary${CSI_Reset}")
    add_executable(${cc_NAME})
    list(APPEND all_targets ${cc_NAME})
    list(APPEND all_targets_not_interface ${cc_NAME})
    add_test(${cc_NAME} ${cc_NAME})
    install(TARGETS ${cc_NAME} DESTINATION bin)
  else ()
    message(FATAL_ERROR "${CSI_BoldRed}Unknown type: ${TYPE_UPPER}${CSI_Reset}")
  endif ()

  message(STATUS "${hint_prefix} - ${CSI_Yellow}Targets: ${all_targets}.${CSI_Reset}")

  cc_check_features(${cc_NAME})
  if (cc_VISIBILITY)
    foreach (target IN LISTS all_targets)
      set_target_properties(${target} PROPERTIES C_VISIBILITY_PRESET ${cc_VISIBILITY})
      set_target_properties(${target} PROPERTIES CXX_VISIBILITY_PRESET ${cc_VISIBILITY})
    endforeach ()
    message(STATUS "{hint_prefix} - ${CSI_Yellow}Visibility: ${cc_VISIBILITY}.${CSI_Reset}")
  endif ()

  foreach (target IN LISTS all_targets)
    target_sources(${target} PRIVATE ${SOURCE_FILES})
    message(STATUS "${hint_prefix} - ${CSI_Yellow}Sources: ${SOURCE_FILES}.${CSI_Reset}")
  endforeach ()

  if (cc_HEADERS)
    set(HEADER_FILES)
    cc_glob(HEADER_FILES cc_HEADERS)
    cc_glob(HEADER_FILES cc_EXCLUDE_HEADERS EXCLUDE)

    foreach (header IN LISTS cc_HEADERS)
      set(CURRENT_HEADER_FILES)
      string(FIND "${header}" "include/" INCLUDE_INDEX)
      if (NOT ${INCLUDE_INDEX} EQUAL -1)
        math(EXPR INCLUDE_END_INDEX "${INCLUDE_INDEX} + 7")
        string(SUBSTRING "${header}" 0 ${INCLUDE_END_INDEX} EXTRACTED_PATH)
        cc_glob(CURRENT_HEADER_FILES header)
        cc_glob(CURRENT_HEADER_FILES cc_EXCLUDE_HEADERS EXCLUDE)
        foreach (target IN LISTS all_targets)
          target_sources(${target} INTERFACE FILE_SET HEADERS BASE_DIRS ${EXTRACTED_PATH}/${cc_NAME} FILES ${CURRENT_HEADER_FILES})
        endforeach ()
      else ()
        cc_glob(CURRENT_HEADER_FILES header)
        cc_glob(CURRENT_HEADER_FILES cc_EXCLUDE_HEADERS EXCLUDE)
        foreach (target IN LISTS all_targets)
          target_sources(${target} INTERFACE FILE_SET HEADERS BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} FILES ${CURRENT_HEADER_FILES})
        endforeach ()
      endif ()
    endforeach()
    install(TARGETS ${all_targets} FILE_SET HEADERS DESTINATION include/${cc_NAME})
  endif ()

  if (cc_INCLUDES)
    foreach (target IN LISTS all_targets)
      get_target_property(TARGET_TYPE ${target} TYPE)
      if ("${TARGET_TYPE}" STREQUAL "INTERFACE_LIBRARY")
        target_include_directories(${target} INTERFACE ${cc_INCLUDES})
      else ()
        target_include_directories(${target} PUBLIC ${cc_INCLUDES})
      endif ()
    endforeach ()
  endif ()

  if (cc_OPTIONS)
    foreach (target IN LISTS all_targets)
      target_compile_options(${target} PRIVATE ${cc_OPTIONS})
    endforeach ()
  endif ()

  if (cc_DEFINITIONS)
    foreach (target IN LISTS all_targets)
      target_compile_definitions(${target} PRIVATE ${cc_DEFINITIONS})
    endforeach ()
  endif ()

  if (cc_LINK_OPTIONS)
    foreach (target IN LISTS all_targets)
      target_link_options(${target} PRIVATE ${cc_LINK_OPTIONS})
    endforeach ()
  endif ()

  get_target_property(aliased ${cc_ALIAS} ALIASED_TARGET)
  if (aliased STREQUAL "${cc_ALIAS}-NOTFOUND")
    target_link_options(${cc_ALIAS} PRIVATE $<$<COMPILE_LANG:C,CXX>:-Wl,-z,noexecstack>)
  endif ()

  if (cc_EXCLUDE_FROM_ALL)
    foreach (target IN LISTS all_targets)
      set_property(TARGET ${target} PROPERTY EXCLUDE_FROM_ALL ON)
    endforeach ()
  endif ()

  # Enable position-independent code defaultly. This is needed because some library targets are OBJECT libraries.
  message(STATUS "${hint_prefix} - ${CSI_Blue}Position Independent Code${CSI_Reset}")
  foreach (target IN LISTS all_targets)
    set_property(TARGET ${target} PROPERTY POSITION_INDEPENDENT_CODE ON)
  endforeach ()

  # Option: Whole achrive
  if (cc_ALWAYS_LINK)
    foreach (target IN LISTS all_targets)
      set_property(TARGET ${target} PROPERTY LINK_BEHAVIOR WHOLE_ARCHIVE)
    endforeach ()
  endif ()
  foreach (depend ${cc_DEPENDENCIES})
    foreach (target IN LISTS all_targets)
      cc_deps(${target} ${depend})
    endforeach ()
  endforeach ()

  # libunwind: a portable and efficient C programming interface (API) to determine the call-chain of a program
  if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
    find_package(PkgConfig)
    if (PkgConfig_FOUND)
      PKG_CHECK_MODULES(LIBUNWIND libunwind-generic)
      if (LIBUNWIND_FOUND)
        foreach (target IN LISTS all_targets)
          target_compile_definitions(${target} PRIVATE -DHAVE_LIBUNWIND)
        endforeach ()
      else ()
        message(STATUS "{hint_prefix} - ${CSI_Yellow}libunwind not found. Disabling unwind tests.${CSI_Reset}")
      endif ()
    else ()
      message(STATUS "{hint_prefix} - ${CSI_Yellow}pkgconfig not found. Disabling unwind tests.${CSI_Reset}")
    endif ()
  endif ()

  foreach (target IN LISTS all_targets_not_interface)
    # target_compile_options(${target} PRIVATE $<IF:$<CONFIG:Debug>,-O0 -g3,-O2 -g>)
    target_compile_definitions(${target} PRIVATE $<IF:$<CONFIG:Debug>,__DEBUG__,__RELEASE__ NDEBUG>)

    target_compile_options(${target} PRIVATE -fomit-frame-pointer)
  endforeach ()

  if (FEATURES_FUZZ)
    message(STATUS "${hint_prefix} - ${CSI_Blue}Fuzz enabled${CSI_Reset}")
    if (NOT CMAKE_COMPILER_IS_CLANG)
      message(FATAL_ERROR "You need to build with Clang for fuzzing to work")
    endif ()

    if (CMAKE_C_COMPILER_VERSION VERSION_LESS "6.0.0")
      message(FATAL_ERROR "You need Clang ≥ 6.0.0")
    endif ()

    foreach (target IN LISTS all_targets_not_interface)
      target_compile_definitions(${target} PRIVATE -DUNSAFE_DETERMINISTIC_MODE)
    endforeach ()
    set(RUNNER_CC "-deterministic")

    if (NOT FEATURES_NO_FUZZER_MODE)
      foreach (target IN LISTS all_targets_not_interface)
        target_compile_definitions(${target} PRIVATE -DUNSAFE_FUZZER_MODE)
      endforeach ()
      set(RUNNER_CC ${RUNNER_CC} "-fuzzer" "-shim-config" "fuzzer_mode.json")
    endif ()

    foreach (target IN LISTS all_targets_not_interface)
      target_compile_options(
        ${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:-fsanitize=address,fuzzer-no-link
                                  -fsanitize-coverage=edge,indirect-calls>
      )
    endforeach ()
  endif ()

  if (FEATURES_MSAN)
    if (FEATURES_ASAN)
      message(FATAL_ERROR "${CSI_BoldRed}ASAN and MSAN are mutually exclusive${CSI_Reset}")
    endif ()

    foreach (target IN LISTS all_targets_not_interface)
      target_compile_options(
        ${target}
        PRIVATE $<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang>:-fsanitize=memory -fsanitize-memory-track-origins
                -fno-omit-frame-pointer> $<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang>:-fsanitize=memory
                -fsanitize-memory-track-origins -fno-omit-frame-pointer>
      )
    endforeach ()
  endif ()

  if (FEATURES_ASAN)
    if (FEATURES_MSAN)
      message(FATAL_ERROR "${CSI_BoldRed}ASAN and MSAN are mutually exclusive${CSI_Reset}")
    endif ()

    foreach (target IN LISTS all_targets_not_interface)
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

  if (FEATURES_DEB)
    foreach (target IN LISTS all_targets_not_interface)
      target_compile_options(${target} PRIVATE -g)
    endforeach ()
  endif ()

  # ROP(Return-oriented Programming) Attack
  if (FEATURES_CFI)
    if (NOT CMAKE_COMPILER_IS_CLANG)
      message(FATAL_ERROR "${CSI_BoldRed}Cannot enable CFI unless using Clang${CSI_Reset}")
    endif ()

    foreach (target IN LISTS all_targets_not_interface)
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

  if (FEATURES_TSAN)
    if (NOT CMAKE_COMPILER_IS_CLANG)
      message(FATAL_ERROR "${CSI_BoldRed}Cannot enable TSAN unless using Clang${CSI_Reset}")
    endif ()

    foreach (target IN LISTS all_targets_not_interface)
      target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:-fsanitize=thread>)

      target_link_options($<$<STREQUAL:$<TARGET_PROPERTY:${target},TYPE>,"EXECUTABLE">:-fsanitize=thread>)
    endforeach ()
  endif ()

  if (FEATURES_UBSAN)
    if (NOT CMAKE_COMPILER_IS_CLANG)
      message(FATAL_ERROR "${CSI_BoldRed}Cannot enable UBSAN unless using Clang${CSI_Reset}")
    endif ()

    foreach (target IN LISTS all_targets_not_interface)
      target_compile_options(
        ${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:-fsanitize=undefined -fsanitize=float-divide-by-zero
                                  -fsanitize=float-cast-overflow -fsanitize=integer>
      )

      target_link_options(
        $<$<STREQUAL:$<TARGET_PROPERTY:${target},TYPE>,"EXECUTABLE">:-fsanitize=undefined
        -fsanitize=float-divide-by-zero -fsanitize=float-cast-overflow -fsanitize=integer>
      )

      if (NOT FEATURES_UBSAN_RECOVER)
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:-fno-sanitize-recover=undefined>)

        target_link_options(
          $<$<STREQUAL:$<TARGET_PROPERTY:${target},TYPE>,"EXECUTABLE">: -fno-sanitize-recover=undefined>
        )
      endif ()
    endforeach ()
  endif ()

  # Coverage
  if (FEATURES_GCOV)
    foreach (target IN LISTS all_targets_not_interface)
      target_link_libraries(${target} PRIVATE gcov)
      target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:-fprofile-arcs -ftest-coverage>)
    endforeach ()

    # cmake-format: off
    add_custom_target(
      ${cc_NAME}_gcov
      COMMAND ${CMAKE_COMMAND} -E make_directory reports/coverage
      COMMAND ${CMAKE_MAKE_PROGRAM} test
      COMMAND echo "Generating coverage reports ..."
      COMMAND gcovr -r ${CMAKE_SOURCE_DIR} --html --html-details
              ${CMAKE_BINARY_DIR}/reports/coverage/full.html
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
    # cmake-format: on
  endif ()

  if (cc_LINK_PTHREAD)
    find_package(Threads)
    if (Threads_FOUND)
      foreach (target IN LISTS all_targets)
        target_link_libraries(${target} PUBLIC ${CMAKE_THREAD_LIBS_INIT} ${CMAKE_DL_LIBS})
      endforeach ()
    endif ()
  endif ()

  # redefine __FILE__ after stripping project dir
  foreach (target IN LISTS all_targets_not_interface)
    target_compile_options(${target} PRIVATE -Wno-builtin-macro-redefined)
  endforeach ()
  # Get source files of target
  get_target_property(source_files ${cc_ALIAS} SOURCES)
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
endfunction ()

function (cc_select target)
  add_library(${target} INTERFACE)

  list(LENGTH ARGN length)
  if (length GREATER 0)
    math(EXPR length "${length} - 1")
    foreach (key RANGE 0 ${length} 2)
      math(EXPR value "${key} + 1")
      list(GET ARGN ${key} condition)
      list(GET ARGN ${value} implementation)

      if ((${condition} STREQUAL "DEFAULT") OR (${${condition}}))
        message(STATUS "${CSI_BoldRed}Using ${library_name} = ${implementation}${CSI_Reset}")
        target_link_libraries(${library_name} INTERFACE ${implementation})
        return ()
      endif ()
    endforeach ()
  endif ()
  message(FATAL_ERROR "${CSI_BoldRed}Could not find implementation for ${library_name}${CSI_Reset}")
endfunction ()

function (cc_library)
  cc_targets(LIBRARY ${ARGV})
endfunction ()

function (cc_binary)
  cc_targets(BINARY ${ARGV})
endfunction ()

function (cc_test)
  cc_targets(TEST ${ARGV})
endfunction ()

function (cc_alias AliasTarget ActualTarget)
  if (NOT TARGET ${AliasTarget})
    add_library(${AliasTarget} ALIAS ${ActualTarget})
  endif ()
endfunction ()

function (cc_source_if target condition)
  if (${condition})
    target_sources(${target} PRIVATE ${ARGN})
  endif ()
endfunction ()

function (cc_module)
  cc_get_namespace(CURRENT_SUPERIOR_NAMESPACE)
  get_filename_component(CURRENT_DIR ${CMAKE_CURRENT_SOURCE_DIR} NAME)
  set(CURRENT_TARGET ${CURRENT_SUPERIOR_NAMESPACE}::${CURRENT_DIR})
  string(REPLACE "::" "_" CURRENT_TARGET ${CURRENT_TARGET})

  cc_library(
    NAME ${CURRENT_TARGET}
    SHARED ON
    NAMESPACE ${CURRENT_DIR}
    ALIAS ${CURRENT_SUPERIOR_NAMESPACE}::${CURRENT_DIR}
    ${ARGV}
  )

  cc_glob(TEST_FILES **_test.cc)
  if (CC_BUILD_TESTS AND TEST_FILES AND COMMAND add_gtest_target)
    add_gtest_target(TEST_TARGET ${CURRENT_TARGET} TEST_SRC ${TEST_FILES})
  endif ()
endfunction ()
