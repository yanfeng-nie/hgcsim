# =============================================================================
# 源码指纹：判断当前源码相对“原始发布状态”是否被修改
#   指纹 = SHA256( 按路径排序的 "相对路径:文件SHA256\n" 拼接 )
#   覆盖：CMakeLists.txt、include/hgcsim/*、src/*、plugins/*.cpp（不含 plugins/student/）
# 用法：
#   include(cmake/fingerprint.cmake) 后调用 hgc_source_fingerprint(<root> <输出变量>)
#   cmake -DROOT=<仓库根目录> -DWRITE=1 -P cmake/fingerprint.cmake   # 重写 scripts/pristine.fingerprint
#   cmake -DROOT=<仓库根目录> -P cmake/fingerprint.cmake             # 打印当前指纹
# Python 前端用完全相同的算法复算指纹（frontend/hgcsim_frontend/buildinfo.py）。
# =============================================================================
function(hgc_source_fingerprint root out)
  file(GLOB files RELATIVE ${root}
       ${root}/CMakeLists.txt
       ${root}/include/hgcsim/*.hpp ${root}/include/hgcsim/*.h
       ${root}/src/*.cpp
       ${root}/plugins/*.cpp)
  list(SORT files)
  set(acc "")
  foreach(f ${files})
    file(SHA256 ${root}/${f} h)
    string(APPEND acc "${f}:${h}\n")
  endforeach()
  string(SHA256 fp "${acc}")
  set(${out} ${fp} PARENT_SCOPE)
endfunction()

function(hgc_student_fingerprint root out)
  file(GLOB files RELATIVE ${root} ${root}/plugins/student/*.cpp)
  list(SORT files)
  set(acc "")
  foreach(f ${files})
    file(SHA256 ${root}/${f} h)
    string(APPEND acc "${f}:${h}\n")
  endforeach()
  string(SHA256 fp "${acc}")
  set(${out} ${fp} PARENT_SCOPE)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE AND DEFINED ROOT)
  hgc_source_fingerprint(${ROOT} fp)
  if(WRITE)
    file(WRITE ${ROOT}/scripts/pristine.fingerprint "${fp}\n")
    message("已写入 scripts/pristine.fingerprint: ${fp}")
  else()
    message("${fp}")
  endif()
endif()
