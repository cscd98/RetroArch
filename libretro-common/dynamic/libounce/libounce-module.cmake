# Call after creating the core's shared-library target.
enable_language(ASM)

function(libounce_add_module target)
  set(module_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
  target_sources(${target} PRIVATE
    "${module_dir}/start.S"
    "${module_dir}/runtime.c")
  target_compile_definitions(${target} PRIVATE LIBOUNCE_MODULE_TLS)
  target_compile_options(${target} PRIVATE -mtls-dialect=trad -ftls-model=global-dynamic)
  target_include_directories(${target} PRIVATE "${module_dir}/../../include")
  set_source_files_properties("${module_dir}/runtime.c" PROPERTIES COMPILE_OPTIONS "-std=gnu11")
  target_link_options(${target} PRIVATE
    -nostdlib -Wl,-Bsymbolic,--hash-style=sysv,-z,text
    "-Wl,-T,$ENV{DEVKITPRO}/libounce/switch.ld"
    "-Wl,-T,${module_dir}/module-tls.ld")
  target_link_libraries(${target} PRIVATE gcc)
  set_target_properties(${target} PROPERTIES PREFIX "" SUFFIX ".elf")
  set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
    "${module_dir}/module-tls.ld" "$ENV{DEVKITPRO}/libounce/switch.ld")
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "$ENV{DEVKITPRO}/tools/bin/elf2nro"
      "$<TARGET_FILE:${target}>" "$<TARGET_FILE_DIR:${target}>/${target}.nro"
    BYPRODUCTS "${CMAKE_BINARY_DIR}/${target}.nro"
    VERBATIM)
endfunction()
