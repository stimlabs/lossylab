# Shared warning flags. Applied per target rather than globally so that any
# dependency added later is not compiled under our settings.

function(lossylab_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wshadow
                -Wconversion
                -Wsign-conversion
                -Wold-style-cast
                -Wnon-virtual-dtor
                -Woverloaded-virtual
        )
    endif()
endfunction()
