function(holyc_configure_global_compiler_options)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-fno-omit-frame-pointer)
  endif()
endfunction()

function(holyc_apply_target_warnings target_name)
  if(MSVC)
    target_compile_options(${target_name} PRIVATE /W4)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
      ${target_name}
      PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wshadow
      -Wundef
      -Wdouble-promotion
    )
  endif()
endfunction()

function(holyc_target_warnings_as_errors target_name)
  if(MSVC)
    target_compile_options(${target_name} PRIVATE /WX)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${target_name} PRIVATE -Werror)
  endif()
endfunction()
