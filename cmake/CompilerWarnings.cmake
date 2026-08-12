# Centralised warning configuration. Applied through hwsim_set_target_warnings()
# so that third-party and generated targets can opt out.

function(hwsim_set_target_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-        # standard conformance, required by several C++20 constructs
            /Zc:__cplusplus     # otherwise __cplusplus is stuck at 199711L
            /Zc:preprocessor    # conforming preprocessor, needed by variadic registration macros
            /utf-8              # sources and execution charset are UTF-8 (project paths may be non-ASCII)
            /bigobj
            /wd4251             # 'X needs dll-interface' - benign for our internal-only DLL boundary
            /wd4275
        )
        if(HWSIM_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wnon-virtual-dtor
            -Wold-style-cast
            -Woverloaded-virtual
            -Wnull-dereference
            -Wdouble-promotion
        )
        if(HWSIM_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
