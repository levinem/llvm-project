set(LLVM_LINK_COMPONENTS
  Support
  )

add_clang_library(clangTemplateCache
  ASTImporterCacheBackend.cpp
  CompositeCacheBackend.cpp
  DaemonCacheBackend.cpp

  LINK_LIBS
  clangAST
  clangBasic
  clangFrontend
  clangLex
  clangSema
  clangSerialization

  DEPENDS
  omp_gen
  ClangDriverOptions
  )

# Link LLVM's HTTP client for daemon GET requests when curl is available.
# DaemonCacheBackend guards all curl usage with #ifdef LLVM_ENABLE_CURL.
if(LLVM_ENABLE_CURL)
  target_link_libraries(clangTemplateCache PRIVATE LLVMSupportHTTP)
endif()
