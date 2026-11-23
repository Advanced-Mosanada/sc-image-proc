# AddressSanitizer + UndefinedBehaviorSanitizer for host debug builds.
# Applied globally so every object (including GoogleTest) is instrumented.
if(SCIP_ENABLE_SANITIZERS)
    if(CMAKE_CROSSCOMPILING)
        message(WARNING "SCIP_ENABLE_SANITIZERS is ignored when cross-compiling "
                        "(the board image has no sanitizer runtimes)")
    else()
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                            -fno-sanitize-recover=undefined)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()
